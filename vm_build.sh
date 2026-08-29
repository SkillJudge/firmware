#!/bin/bash
#
# vm_build.sh
# --------------------------------------------------------------
# 在你的虚机里执行完整打包（与 build.sh 等价，但不会 git pull，
# 避免你本地修改被上游覆盖；需要拉代码时你自己手动 git pull）。
#
# 步骤：
#   1. 先跑 bash ./vm_preflight.sh 预检
#   2. 执行 ./add_exec.sh
#   3. cp myconfig -> output/.config （同 build.sh 步骤 1）
#   4. make savedefconfig 生成 openipc_defconfig （同 build.sh 步骤 2）
#   5. cp defconfig -> br-ext-chip-goke/configs/my_defconfig
#   6. 调用 make BOARD=my all （不带 clean，做增量编译，省时间；
#      想彻底 clean 的话请带 --clean 参数）
#
# 用法：
#   cd /path/to/firmware
#   bash ./vm_build.sh                  # 增量构建（推荐默认）
#   bash ./vm_build.sh --clean          # 先 clean 再 all
#   bash ./vm_build.sh --logs /tmp/b.log 2>&1 | tee /tmp/b.log
#
# --------------------------------------------------------------
set -u
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR" || exit 1

CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1 ;;
        *) echo "未知参数: $1"; echo "用法: $0 [--clean]"; exit 2 ;;
    esac
    shift
done

BOLD_GREEN=$'\033[1;32m'
BOLD_RED=$'\033[1;31m'
BOLD_YELLOW=$'\033[1;33m'
BOLD_CYAN=$'\033[1;36m'
RESET=$'\033[0m'

ts_now()   { date '+%F %T'; }
log_title() { echo -e "\n${BOLD_CYAN}===== [$(ts_now)] $* =====${RESET}"; }
log_ok()    { echo -e "  ${BOLD_GREEN}[OK]${RESET}  $*"; }
log_fail()  { echo -e "  ${BOLD_RED}[FAIL]${RESET} $*"; }
log_warn()  { echo -e "  ${BOLD_YELLOW}[WARN]${RESET} $*"; }

START_TS=$(date '+%s')

log_title "Step 0: preflight 预检"
if ! bash ./vm_preflight.sh; then
    log_fail "preflight 未通过，打包中止。先修上面的问题再跑"
    exit 1
fi
log_ok "preflight 通过"

log_title "Step 1: 执行 add_exec.sh"
if bash ./add_exec.sh; then
    log_ok "add_exec.sh 完成"
else
    log_fail "add_exec.sh 失败，中止"
    exit 1
fi

log_title "Step 2: myconfig -> output/.config"
if [ ! -f myconfig ]; then
    log_fail "myconfig 文件缺失，无法继续（参考 build.sh 的安全检查）"
    exit 1
fi
cp -v myconfig output/.config || { log_fail "copy myconfig 失败"; exit 1; }
log_ok "output/.config 已覆盖"

log_title "Step 3: 生成精简 defconfig (savedefconfig)"
if [ -f output/Makefile ]; then
    (
        cd output
        make savedefconfig || { log_fail "output/Makefile savedefconfig 失败"; exit 1; }
    ) || exit 1
else
    log_warn "output/Makefile 不存在，尝试用顶层 make BOARD=my savedefconfig"
    make BOARD=my savedefconfig || { log_fail "顶层 savedefconfig 失败"; exit 1; }
fi

if [ -s output/openipc_defconfig ]; then
    log_ok "output/openipc_defconfig 已生成（非空）"
else
    log_fail "output/openipc_defconfig 未生成或为空（build.sh 步骤 4 里的强制校验）"
    exit 1
fi

log_title "Step 4: 拷贝 defconfig -> br-ext-chip-goke/configs/my_defconfig"
mkdir -p br-ext-chip-goke/configs
cp -v output/openipc_defconfig br-ext-chip-goke/configs/my_defconfig || { log_fail "拷贝失败"; exit 1; }
log_ok "my_defconfig 已更新"

log_title "Step 5: 执行 make BOARD=my ${CLEAN:+clean }all（这一步最长，请耐心等）"
if [ "$CLEAN" -eq 1 ]; then
    make BOARD=my clean all
    rc=$?
else
    make BOARD=my all
    rc=$?
fi
END_TS=$(date '+%s')
ELAPSED=$(( END_TS - START_TS ))
ELAPSED_MIN=$(( ELAPSED / 60 ))
ELAPSED_SEC=$(( ELAPSED % 60 ))

echo
if [ "$rc" -eq 0 ]; then
    log_ok "构建成功，耗时 ${ELAPSED_MIN}m${ELAPSED_SEC}s"
    echo -e "${BOLD_CYAN}固件产物目录: $(pwd)/output/images/${RESET}"
    ls -lh output/images/*.bin output/images/*.rootfs.* 2>/dev/null || true
    echo
    echo "下一步常见操作："
    echo "  - 把 output/images/openipc-gk7205v300-ultimate-16mb.bin 拷到 Windows 的 tftp/ 目录"
    echo "  - 用 ToolPlatform 按分区烧录；或网线直连跑 tftp 升级"
    exit 0
else
    log_fail "构建失败（rc=$rc），耗时 ${ELAPSED_MIN}m${ELAPSED_SEC}s"
    echo "   请把 output/build/ 下对应包的 log 或 make 输出的最后 200 行贴回给我排查。"
    exit "$rc"
fi
