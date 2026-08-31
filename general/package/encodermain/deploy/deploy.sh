#!/usr/bin/env bash
# ========================================================================
# deploy.sh — encodermain 快速迭代部署脚本
# （在编译虚机 192.168.0.104 上执行，因为 buildroot / toolchain 只有那里有）
#
# 用法：
#   1) cd firmware/general/package/encodermain/deploy
#   2) export 必需环境变量（见下方 CHECKLIST 或 README_deploy.md）
#   3) bash deploy.sh
#      # 或只做其中某一步：
#      bash deploy.sh --no-build       # 跳过 build, 只 copy + run (之前已 build 过)
#      bash deploy.sh --no-run         # build + copy 到 SD, 但不 runmain
#      bash deploy.sh --no-copy        # 只 build
#      bash deploy.sh --restore        # 只在目标机跑 ./runmain restore
#
# 流程四步：
#   [1] cd $GIT_DIR (即 firmware/ 同级 repo root)  →  git pull --ff-only
#   [2] firmware/ 下 make BOARD=$FIRMWARE_BOARD br-encodermain-rebuild
#       （等价于 buildroot generic package: dirclean + rebuild + install-target）
#       产物出现在 $OUTPUT_DIR/target/usr/sbin/encodermain
#   [3] 通过 58.198.176.157 做 SSH 跳板（ProxyJump），scp 到 BOARD_IP
#       的 /mnt/mmcblk0p1/encdeploy/，文件包括：
#         encodermain, default.encoder, S96encodermain, config.sh(optional),
#         runmain
#   [4] 远端 SSH 执行：chmod +x ./runmain + ./runmain（停旧启新）
#
# 必需 env vars：
#   GIT_DIR         本机 git repo 根（包含 firmware/, ctrlserver/ 等的那个 dir）
#   FIRMWARE_BOARD  板级 defconfig，如 gk7205v300-nor-ultimate
#   JUMP_HOST       跳板机，默认 58.198.176.157
#   JUMP_USER       跳板机 ssh 用户名
#   JUMP_PASS       跳板机 ssh 密码  (若配了免密可任意非空占位符)
#   BOARD_IP        编码器 01 号 可从跳板机直连的 IP
#   BOARD_USER      编码器 ssh 用户名，默认 root
#   BOARD_PASS      编码器 ssh 密码
#
# 可选 env vars：
#   OUTPUT_DIR      buildroot 输出目录，默认 $GIT_DIR/firmware/output
#   FIRMWARE_DIR    默认 $GIT_DIR/firmware
#   DEPLOY_SRCDIR   deploy/ 源文件目录，默认 $(dirname $0)/.. (encodermain package)
#   BOARD_DEPLOYDIR 远端 SD 卡部署目录，默认 /mnt/mmcblk0p1/encdeploy
#   USE_SSHPASS     1/0  是否强制用 sshpass；默认自动检测 sshpass 是否安装
# ========================================================================
set -u -o pipefail

# ---------- env & default ----------
: "${GIT_DIR:?ERROR: GIT_DIR 未设置；请 export GIT_DIR=/path/to/git/repo}"
: "${FIRMWARE_BOARD:?ERROR: FIRMWARE_BOARD 未设置；示例: export FIRMWARE_BOARD=gk7205v300-nor-ultimate}"
: "${JUMP_HOST:=58.198.176.157}"
: "${JUMP_USER:?ERROR: JUMP_USER 未设置 (跳板机登录用户名)}"
: "${JUMP_PASS:=nopass}"
: "${BOARD_IP:?ERROR: BOARD_IP 未设置 (编码器从跳板机可直连的 IP)}"
: "${BOARD_USER:=root}"
: "${BOARD_PASS:?ERROR: BOARD_PASS 未设置 (编码器 SSH 密码)}"

: "${FIRMWARE_DIR:=$GIT_DIR/firmware}"
: "${OUTPUT_DIR:=$FIRMWARE_DIR/output}"
DEPLOY_SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"  # encodermain package dir
: "${BOARD_DEPLOYDIR:=/mnt/mmcblk0p1/encdeploy}"

FLAG_BUILD=1; FLAG_COPY=1; FLAG_RUN=1; FLAG_RESTORE=0
for a in "$@"; do
  case "$a" in
    --no-build) FLAG_BUILD=0 ;;
    --no-copy)  FLAG_COPY=0 ;;
    --no-run)   FLAG_RUN=0  ;;
    --restore)  FLAG_RESTORE=1; FLAG_BUILD=0; FLAG_COPY=0 ;;
    -h|--help)
      sed -n '2,60p' "$0"; exit 0 ;;
    *) echo "Unknown arg: $a (try --help)"; exit 2 ;;
  esac
done

# ---------- tooling ----------
HAS_SSHPASS=0
command -v sshpass >/dev/null 2>&1 && HAS_SSHPASS=1
if [ "${USE_SSHPASS:-auto}" = auto ]; then
  USE_SSHPASS=$HAS_SSHPASS
fi
if [ "$USE_SSHPASS" = 0 ] && [ "$JUMP_PASS" != "nopass" -o "$BOARD_PASS" != "nopass" ]; then
  echo "WARNING: 未安装 sshpass 但您设置了密码。请先:  apt install -y sshpass （推荐）"
  echo "         或者使用 ssh-agent + 免密 key。当前将尝试忽略密码，如果卡住请 C-c。"
fi

_SSH_COMMON_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 -o ServerAliveInterval=30 -o LogLevel=ERROR"

log()  { printf "[deploy %s] %s\n" "$(date '+%H:%M:%S')" "$*"; }
fail() { printf "[deploy FATAL %s] %s\n" "$(date '+%H:%M:%S')" "$*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || fail "缺少依赖命令: $1 (请 apt install -y $1)"; }

# ---------- step 1/4 dependencies ----------
need git
need make
need ssh
need scp
need awk
need sed

if [ "$USE_SSHPASS" = 1 ]; then need sshpass; fi

# ---------- step 2/4 git pull ----------
[ -d "$GIT_DIR/.git" ] || fail "GIT_DIR=$GIT_DIR 不是 git 仓库 (.git dir 不存在)"
[ -d "$FIRMWARE_DIR"  ] || fail "FIRMWARE_DIR=$FIRMWARE_DIR 不存在；检查 GIT_DIR 是否指到 repo 根"
[ -f "$FIRMWARE_DIR/Makefile" ] || fail "$FIRMWARE_DIR/Makefile 不存在；FIRMWARE_DIR=$FIRMWARE_DIR 不对"

log "[step1] git pull @ $GIT_DIR"
(
  cd "$GIT_DIR"
  # Stash local dirty only if dirty (allow user leave temp edits)
  dirty=0; git diff --quiet 2>/dev/null || dirty=1
  if [ $dirty -eq 1 ]; then
    log "  本地有未提交改动，先 git stash push -m deploy-auto-stash；执行完会 git stash pop"
    git stash push -u -m "deploy-auto-stash $(date)" || dirty=0
  fi
  git pull --ff-only || fail "git pull --ff-only 失败（存在冲突或未推送本地分支）；请手工 git pull 解决后再跑"
  if [ $dirty -eq 1 ]; then git stash pop 2>/dev/null || log "  stash pop 提示 (忽略此消息); 请检查冲突文件"; fi
  git log -1 --oneline
) || fail "git pull 失败"

# ---------- step 3/4 make encodermain ----------
BINARY="$OUTPUT_DIR/target/usr/sbin/encodermain"
if [ $FLAG_BUILD -eq 1 ]; then
  log "[step2] build encodermain package (BOARD=$FIRMWARE_BOARD)"
  log "  make -C $FIRMWARE_DIR BOARD=$FIRMWARE_BOARD br-encodermain-rebuild"
  (
    cd "$FIRMWARE_DIR"
    # make BOARD=... all 必须先跑一次（buildroot download + defconfig）确保可用；
    # 此处直接尝试 rebuild；若失败提示用户
    make BOARD="$FIRMWARE_BOARD" br-encodermain-dirclean || true
    make BOARD="$FIRMWARE_BOARD" br-encodermain-rebuild -j"$(nproc 2>/dev/null || echo 4)" 2>&1 \
      | tail -60
  ) || fail "buildroot br-encodermain-rebuild 失败，请查看上面 tail 日志"
  [ -x "$BINARY" ] || fail "build 成功但找不到产物 $BINARY"
  log "  build OK: $BINARY  ($(stat -c %s "$BINARY") bytes, $(file -b "$BINARY" 2>/dev/null | head -1))"
else
  [ -x "$BINARY" ] || fail "跳过 build，但是 $BINARY 不存在；请去掉 --no-build 先 build 一次"
  log "[step2] SKIP build (--no-build), use existing binary $BINARY"
fi

# ---------- helpers for jump-hosted scp/ssh ----------
# We stage files into a temp dir on jump, then scp from jump -> encoder, then ssh from jump -> encoder runmain.
JUMP_TMP="/tmp/encdeploy.$$"
cleanup() {
  log "cleanup: rm -rf $JUMP_TMP @ jump"
  if [ "$USE_SSHPASS" = 1 ]; then
    SSHPASS="$JUMP_PASS" sshpass -e ssh ${_SSH_COMMON_OPTS} \
      "${JUMP_USER}@${JUMP_HOST}" "rm -rf '$JUMP_TMP'" 2>/dev/null || true
  else
    ssh ${_SSH_COMMON_OPTS} "${JUMP_USER}@${JUMP_HOST}" "rm -rf '$JUMP_TMP'" 2>/dev/null || true
  fi
}
trap cleanup EXIT

dossh_jump() {
  # run command on jump host
  local c="$1"
  if [ "$USE_SSHPASS" = 1 ]; then
    SSHPASS="$JUMP_PASS" sshpass -e ssh ${_SSH_COMMON_OPTS} "${JUMP_USER}@${JUMP_HOST}" "$c"
  else
    ssh ${_SSH_COMMON_OPTS} "${JUMP_USER}@${JUMP_HOST}" "$c"
  fi
}

# scp a local file to jump:$JUMP_TMP/
doscp_tojump() {
  local localf="$1"; local bn
  bn="$(basename "$localf")"
  if [ "$USE_SSHPASS" = 1 ]; then
    SSHPASS="$JUMP_PASS" sshpass -e scp ${_SSH_COMMON_OPTS} "$localf" "${JUMP_USER}@${JUMP_HOST}:${JUMP_TMP}/${bn}"
  else
    scp ${_SSH_COMMON_OPTS} "$localf" "${JUMP_USER}@${JUMP_HOST}:${JUMP_TMP}/${bn}"
  fi
}

# run command on encoder (via jump) using inner sshpass with BOARD_PASS
dossh_board() {
  local c="$1"
  # Write script into jump tmp, then run it via jump ssh (which uses sshpass to encoder)
  # The script on jump:
  cat >/tmp/_encdeploy_inner.$$.sh <<EOF
#!/bin/bash
set -u
export SSHPASS='${BOARD_PASS}'
sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  -o ConnectTimeout=15 -o PreferredAuthentications=password,keyboard-interactive,password \
  -o PubkeyAuthentication=no \
  '${BOARD_USER}@${BOARD_IP}' \
  '$(printf '%q' "$c")'
rc=\$?
exit \$rc
EOF
  chmod +x /tmp/_encdeploy_inner.$$.sh
  doscp_tojump /tmp/_encdeploy_inner.$$.sh >/dev/null
  dossh_jump "bash '$JUMP_TMP/_encdeploy_inner.$$.sh'" ; local rc=$?
  rm -f /tmp/_encdeploy_inner.$$.sh
  return $rc
}

# scp from jump staging dir to encoder via jump ssh + sshpass scp
doscp_board_fromstaged() {
  # $1 = staged filename inside jump $JUMP_TMP
  local staged="$1"
  cat >/tmp/_encdeploy_cp.$$.sh <<EOF
#!/bin/bash
set -u
export SSHPASS='${BOARD_PASS}'
sshpass -e scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  -o PreferredAuthentications=password,keyboard-interactive,password \
  -o PubkeyAuthentication=no \
  '$JUMP_TMP/${staged}' '${BOARD_USER}@${BOARD_IP}:${BOARD_DEPLOYDIR}/${staged}'
rc=\$?
exit \$rc
EOF
  chmod +x /tmp/_encdeploy_cp.$$.sh
  doscp_tojump /tmp/_encdeploy_cp.$$.sh >/dev/null
  dossh_jump "bash '$JUMP_TMP/_encdeploy_cp.$$.sh'" ; local rc=$?
  rm -f /tmp/_encdeploy_cp.$$.sh
  return $rc
}

# ---------- step 4/4 copy + runmain ----------
if [ $FLAG_COPY -eq 1 -o $FLAG_RESTORE -eq 1 ]; then
  log "[step3] 准备跳板机 staging dir: $JUMP_TMP"
  dossh_jump "mkdir -p '$JUMP_TMP'" || fail "跳板机 mkdir $JUMP_TMP 失败"
  log "[step4] 检测编码器 SSH & 建 SD 部署目录 $BOARD_DEPLOYDIR"
  dossh_board "mkdir -p '$BOARD_DEPLOYDIR/logs' && mount | grep -q mmcblk0p1 && echo 'SD_OK' || echo 'WARN_SD_NOT_MOUNTED (deploy will continue but files go to rootfs)' " \
    || fail "编码器 SSH 连不上（请核对 JUMP_*/BOARD_* 密码/IP）"
fi

if [ $FLAG_RESTORE -eq 1 ]; then
  log "[restore] 在编码器上执行: $BOARD_DEPLOYDIR/runmain restore"
  dossh_board "test -x '$BOARD_DEPLOYDIR/runmain' || { echo 'runmain 未部署 请先完整 deploy'; exit 6; }; '$BOARD_DEPLOYDIR/runmain' restore" \
    && log "RESTORE OK" && exit 0
  fail "restore 返回非 0"
fi

if [ $FLAG_COPY -eq 1 ]; then
  log "[step4] 上传 (1) build 产物 encodermain"
  # stage local -> jump
  doscp_tojump "$BINARY"                          || fail "scp encodermain -> jump 失败"
  doscp_board_fromstaged "encodermain"            || fail "scp encodermain jump->board 失败"
  log "[step4] 上传 (2) deploy/runmain"
  doscp_tojump "$DEPLOY_SRCDIR/deploy/runmain"    || fail "scp runmain -> jump 失败"
  doscp_board_fromstaged "runmain"                || fail "scp runmain jump->board 失败"
  log "[step4] 上传 (3) default.encoder, S96encodermain（配置参考/覆盖）"
  doscp_tojump "$DEPLOY_SRCDIR/default.encoder"   || fail "scp default.encoder -> jump 失败"
  doscp_board_fromstaged "default.encoder"        || true
  doscp_tojump "$DEPLOY_SRCDIR/S96encodermain"     || fail "scp S96encodermain -> jump 失败"
  doscp_board_fromstaged "S96encodermain"          || true
  # Optional: config.sh 若用户放在 deploy/ 下也一起传
  if [ -f "$DEPLOY_SRCDIR/deploy/config.sh.override" ]; then
    log "[step4] 上传 (4-opt) deploy/config.sh.override → 远端 config.sh"
    doscp_tojump "$DEPLOY_SRCDIR/deploy/config.sh.override"
    # rename on jump so the scp_board copies it as "config.sh"
    dossh_jump "mv -f '$JUMP_TMP/config.sh.override' '$JUMP_TMP/config.sh'"
    doscp_board_fromstaged "config.sh" || true
  fi
  # chmod executable on board
  log "[step4] 远端 chmod +x encodermain runmain"
  dossh_board "chmod +x '$BOARD_DEPLOYDIR/encodermain' '$BOARD_DEPLOYDIR/runmain'; ls -la '$BOARD_DEPLOYDIR'" \
    || fail "chmod/ls 远端 encdeploy 失败"
fi

if [ $FLAG_RUN -eq 1 ]; then
  log "[step5] 在编码器 01 上执行 runmain → 停固件 encodermain + 启新版本"
  dossh_board "'$BOARD_DEPLOYDIR/runmain' start" \
    && log "DEPLOY & RUN SUCCESS ✓" \
    && exit 0
  fail "远端 runmain start 非 0 返回；请在编码器上 cat $BOARD_DEPLOYDIR/logs/encodermain-*.log 查日志"
fi

log "DONE (deploy.sh 选择的 flag 链执行完毕：BUILD=$FLAG_BUILD COPY=$FLAG_COPY RUN=$FLAG_RUN)"
exit 0
