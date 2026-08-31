#!/usr/bin/env bash
# =============================================================
# run.sh — 【一键入口】在编译虚机 192.168.0.104 上直接 bash run.sh
#          就会：source env.config → deploy.sh
# 附加功能：
#   bash run.sh init        # 首次跑：sudo apt install sshpass + 跳板机 apt install sshpass（需要你有 sudo 密码）
#   bash run.sh status      # 只看远端当前 encodermain 状态、SD 目录内容、日志最后 20 行
#   bash run.sh restore     # 远端 ./runmain restore（回到固件版）
#   bash run.sh stop        # 远端 ./runmain stop（停所有 encodermain）
#   bash run.sh just-run    # 跳过 build/copy，只在远端 runmain start（已部署过想重启用）
# =============================================================
set -u -o pipefail
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"

# --- 1. 加载参数 ---
ENV="$SELF_DIR/env.config"
if [ ! -f "$ENV" ]; then
  echo "[run.sh FATAL] 找不到 $ENV，请先填 env.config（模板已经写好，缺的地方补齐即可）"
  echo "  模板位置 encodermain/deploy/env.config（同目录）"
  exit 2
fi
# shellcheck disable=SC1090
set -a; . "$ENV"; set +a

: "${GIT_DIR:?env.config 里 GIT_DIR 为空，请填}"
: "${JUMP_HOST:?JUMP_HOST 为空}"; : "${JUMP_USER:?JUMP_USER 为空}"; : "${JUMP_PASS:?JUMP_PASS 为空}"
: "${BOARD_IP:?BOARD_IP 为空}";  : "${BOARD_USER:?BOARD_USER 为空}"; : "${BOARD_PASS:?BOARD_PASS 为空}"

CMD="${1:-deploy}"
DEPLOY_SH="$SELF_DIR/deploy.sh"

# --- 2. subcommand ---
case "$CMD" in
init)
  echo "==> 本机(编译虚机): sudo apt install -y sshpass build-essential file git make openssh-client"
  sudo apt update && sudo apt install -y sshpass build-essential file git make openssh-client || true
  echo
  echo "==> 跳板机 $JUMP_USER@$JUMP_HOST: sudo apt install -y sshpass openssh-client"
  SSHPASS="$JUMP_PASS" sshpass -e ssh \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 \
    "${JUMP_USER}@${JUMP_HOST}" \
    "sudo apt update && sudo apt install -y sshpass openssh-client && echo JUMP_OK" \
  && echo "init 完成 ✓"
  exit $?
  ;;
status)
  echo "==> 远端 $BOARD_IP 状态"
  bash "$DEPLOY_SH" --no-build --no-copy --no-run
  # 上面只会做 env 检查；下面实际探活
  # 重用 deploy.sh 的跳板->板SSH函数不方便，这里写一个最小远端执行。
  cat >/tmp/_encstatus.$$.sh <<EOFBASH
#!/bin/bash
set -u
export SSHPASS='${BOARD_PASS}'
sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 \
  -o PreferredAuthentications=password,keyboard-interactive,password -o PubkeyAuthentication=no \
  '${BOARD_USER}@${BOARD_IP}' \
  'echo "---mount sd---"; mount | grep mmcblk0p1 | head -2;
   echo "---encdeploy---"; ls -la "${BOARD_DEPLOYDIR}" 2>&1 | head -20;
   echo "---running encodermain---"; pgrep -a encodermain || echo "(none)";
   echo "---PID file---"; cat /root/encoder/runtime/state/pid 2>&1;
   echo "---latest sd log tail 30---"; LOG=$(ls -1t "${BOARD_DEPLOYDIR}"/logs/encodermain-*.log 2>/dev/null | head -1); echo "LOG=$LOG"; [ -n "$LOG" ] && tail -30 "$LOG" 2>&1'
EOFBASH
  chmod +x /tmp/_encstatus.$$.sh
  # 通过跳板机跑这段脚本（跳板机本身执行 sshpass 到板端）
  JUMPTMP="/tmp/encstatus.$$";
  SSHPASS="$JUMP_PASS" sshpass -e scp \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    /tmp/_encstatus.$$.sh "${JUMP_USER}@${JUMP_HOST}:${JUMPTMP}.sh"
  SSHPASS="$JUMP_PASS" sshpass -e ssh \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 \
    "${JUMP_USER}@${JUMP_HOST}" "bash ${JUMPTMP}.sh; rc=\$?; rm -f ${JUMPTMP}.sh; exit \$rc"
  rm -f /tmp/_encstatus.$$.sh
  exit $?
  ;;
restore) bash "$DEPLOY_SH" --restore; exit $? ;;
stop)
  # 远端只 runmain stop
  cat >/tmp/_encstop.$$.sh <<EOFBASH
#!/bin/bash
set -u
export SSHPASS='${BOARD_PASS}'
sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 \
  -o PreferredAuthentications=password,keyboard-interactive,password -o PubkeyAuthentication=no \
  '${BOARD_USER}@${BOARD_IP}' \
  'test -x "${BOARD_DEPLOYDIR}/runmain" && "${BOARD_DEPLOYDIR}/runmain" stop || echo "runmain 未 deploy 过/不存在"'
EOFBASH
  chmod +x /tmp/_encstop.$$.sh
  JUMPTMP="/tmp/encstop.$$"
  SSHPASS="$JUMP_PASS" sshpass -e scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null /tmp/_encstop.$$.sh "${JUMP_USER}@${JUMP_HOST}:${JUMPTMP}.sh"
  SSHPASS="$JUMP_PASS" sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 \
    "${JUMP_USER}@${JUMP_HOST}" "bash ${JUMPTMP}.sh; rc=\$?; rm -f ${JUMPTMP}.sh; exit \$rc"
  rm -f /tmp/_encstop.$$.sh
  exit $?
  ;;
just-run)
  # 跳过 build/copy，只在远端 runmain start
  cat >/tmp/_encjustrun.$$.sh <<EOFBASH
#!/bin/bash
set -u
export SSHPASS='${BOARD_PASS}'
sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15 \
  -o PreferredAuthentications=password,keyboard-interactive,password -o PubkeyAuthentication=no \
  '${BOARD_USER}@${BOARD_IP}' \
  'test -x "${BOARD_DEPLOYDIR}/runmain" || { echo "runmain 未部署 先完整 bash run.sh 一次"; exit 6; }; "${BOARD_DEPLOYDIR}/runmain" start'
EOFBASH
  chmod +x /tmp/_encjustrun.$$.sh
  JUMPTMP="/tmp/encjustrun.$$"
  SSHPASS="$JUMP_PASS" sshpass -e scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null /tmp/_encjustrun.$$.sh "${JUMP_USER}@${JUMP_HOST}:${JUMPTMP}.sh"
  SSHPASS="$JUMP_PASS" sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=20 \
    "${JUMP_USER}@${JUMP_HOST}" "bash ${JUMPTMP}.sh; rc=\$?; rm -f ${JUMPTMP}.sh; exit \$rc"
  rm -f /tmp/_encjustrun.$$.sh
  exit $?
  ;;
deploy|build)
  echo "==> RUN deploy (CMD=$CMD)  →  bash $DEPLOY_SH $([ "$CMD" = "build" ] && echo --no-copy --no-run)"
  if [ "$CMD" = "build" ]; then
    bash "$DEPLOY_SH" --no-copy --no-run
  else
    bash "$DEPLOY_SH"
  fi
  exit $?
  ;;
-h|--help|help)
  sed -n '2,15p' "$0"
  echo "
 可用子命令:
   deploy   (默认) 完整: git pull → build → copy → runmain start
   build    只 build（跳过 copy/run）
   init     一次性：编译虚机 + 跳板机 apt install sshpass
   status   远端探活：进程/PID/logs 最后 30 行
   stop     远端 runmain stop（停所有 encodermain，不恢复固件）
   restore  远端 runmain restore（恢复固件 encodermain + 回滚配置）
   just-run 远端 runmain start（build/copy 都跳过；需要先完整 deploy 过一次）
"
  exit 0
  ;;
esac

echo "未知子命令 $CMD（try --help）" >&2
exit 2
