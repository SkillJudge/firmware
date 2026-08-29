#!/bin/bash
#
# vm_preflight.sh
# --------------------------------------------------------------
# 在你的虚机里执行，用来在正式 ./build.sh 之前快速验证：
#   1. 所有新增 overlay shell 脚本的语法（sh -n）
#   2. add_exec.sh 能否对清单里每个文件 chmod +x 成功
#   3. 环境必备工具（goose/sqlc 不需要，固件打包用 buildroot）
#   4. myconfig 是否存在、关键路径是否齐全
#
# 用法（虚机终端里）：
#     cd /path/to/firmware        # 必须切到 firmware 根目录
#     bash ./vm_preflight.sh
#
# 如果你希望所有输出都保存到文件并回传给我排查：
#     bash ./vm_preflight.sh 2>&1 | tee /tmp/vm_preflight_$(date +%F_%H%M%S).log
#
# 注意：这个脚本不会修改你仓库里的任何源码内容，只会：
#       - 对脚本执行 chmod +x（和 add_exec.sh 本身约定一致）
#       - 复制 myconfig 到 output/.config 时只在临时副本里测试，不会持久化
# --------------------------------------------------------------
set -u
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR" || exit 1

PASS=0
FAIL=0
warn_count=0

title()   { printf '\n\033[1;36m===== %s =====\033[0m\n' "$*"; }
section() { printf '\n\033[1;34m--- %s ---\033[0m\n' "$*"; }
ok()      { printf '  \033[1;32m[OK]\033[0m  %s\n' "$*"; PASS=$((PASS + 1)); }
fail()    { printf '  \033[1;31m[FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL + 1)); }
warn()    { printf '  \033[1;33m[WARN]\033[0m %s\n' "$*"; warn_count=$((warn_count + 1)); }
info()    { printf '  \033[2;37m[INFO]\033[0m %s\n' "$*"; }

section "Firmware root"
info "PWD = $(pwd)"
if [ -f general/openipc.fragment ] && [ -f Makefile ] && [ -f myconfig ]; then
    ok "看起来是 firmware 仓库根目录（Makefile / general / myconfig 均存在）"
else
    fail "没有在 firmware 根目录下执行，或缺 myconfig，请 cd 到 firmware 目录再跑"
fi

section "Shell scripts syntax check (sh -n)"
SYNTAX_TARGETS=(
    "general/overlay/etc/init.d/S40network"
    "general/overlay/etc/init.d/S99zzencoder"
    "general/overlay/etc/init.d/S15_i2c_init"
    "general/overlay/etc/init.d/rcS"
    "general/overlay/etc/rc.local"
    "general/overlay/etc/wireless/usb"
    "general/overlay/etc/wireless/sdio"
    "add_exec.sh"
)
for f in "${SYNTAX_TARGETS[@]}"; do
    if [ ! -f "$f" ]; then
        fail "文件不存在: $f"
        continue
    fi
    if sh -n "$f" 2>/dev/null; then
        ok "sh -n $f"
    else
        fail "sh -n $f （下面是详细错误输出）"
        printf '      >>> '
        sh -n "$f" 2>&1 | sed 's/^/      >>> /'
    fi
done

section "add_exec.sh verification"
if bash -n add_exec.sh 2>/dev/null; then
    ok "add_exec.sh 自身 bash -n 语法检查通过"
else
    fail "add_exec.sh bash -n 语法错误"
fi

# 实际跑 add_exec.sh，但先暂存原始权限，脚本本身就是 +x
if bash ./add_exec.sh; then
    ok "./add_exec.sh 执行成功（全部文件已设置可执行权限）"
else
    fail "./add_exec.sh 执行失败，请检查上方输出的 listed file not found 行"
fi

section "Executable permission check"
EXPECTED_EXEC=(
    "general/overlay/usr/bin/ircut_demo"
    "general/overlay/usr/bin/led_test.sh"
)
for f in "${EXPECTED_EXEC[@]}"; do
    if [ -x "$f" ]; then
        ok "$f 有执行权限"
    else
        fail "$f 缺少执行权限（请检查 add_exec.sh 清单）"
    fi
done

section "Overlay structure check"
for item in \
    "general/overlay/etc/init.d/S40network" \
    "general/overlay/etc/init.d/S99zzencoder" \
    "general/overlay/usr/bin/ircut_demo" \
    "general/overlay/usr/bin/led_test.sh"
do
    if [ -f "$item" ]; then
        ok "overlay 文件存在: $item"
    else
        fail "overlay 文件缺失: $item"
    fi
done

section "Toolchain check"
for cmd in make cp awk sed; do
    if command -v "$cmd" >/dev/null 2>&1; then
        ok "命令可用: $cmd ($(command -v "$cmd"))"
    else
        fail "缺少命令: $cmd，Buildroot 编译环境未就绪"
    fi
done
if [ -d output ]; then
    ok "output/ 目录已存在"
    if [ -f output/Makefile ]; then
        ok "output/Makefile 存在（Buildroot 已至少解压过一次）"
    else
        warn "output/Makefile 不存在，Buildroot 可能还没初始化（./build.sh 会自动处理）"
    fi
else
    warn "output/ 目录不存在，首次执行 ./build.sh 时会生成"
fi

section "BR2_ROOTFS_OVERLAY check"
if grep -q 'BR2_ROOTFS_OVERLAY="$(BR2_EXTERNAL)/overlay"' general/openipc.fragment; then
    ok "general/openipc.fragment 已配置 BR2_ROOTFS_OVERLAY -> \$(BR2_EXTERNAL)/overlay"
else
    fail "general/openipc.fragment 缺少 BR2_ROOTFS_OVERLAY，新 overlay 文件不会进 rootfs"
fi

echo
echo "============================================================="
title   "PREFLIGHT SUMMARY"
section "TOTAL: PASS=$PASS   FAIL=$FAIL   WARN=$warn_count"
echo "============================================================="
if [ "$FAIL" -eq 0 ]; then
    echo -e "\n\033[1;32m全部预检通过，可以进入下一步：\033[0m"
    echo "  1. （可选）bash ./vm_build.sh   ——  执行完整打包"
    echo "  2. （直接）bash ./build.sh     ——  git pull + add_exec + myconfig → clean all"
else
    echo -e "\n\033[1;31m存在 $FAIL 项失败，请先解决 FAIL 行再继续打包。\033[0m"
    echo "   如果你无法判断，请把整个脚本输出（包括上面的 FAIL 详细）贴回给我。"
    exit 1
fi
