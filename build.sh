#!/bin/bash
# 确保脚本在遇到任何错误时立刻退出，防止错误扩大
set -e

git pull

chmod +x ./add_exec.sh

./add_exec.sh

echo "=================================================="
echo "开始执行 OpenIPC 音频配置更新与固件编译流程 (带安全校验)"
echo "=================================================="

# 编译前读取固件版本号（来自 general/overlay/etc/version）
VERSION_FILE="./general/overlay/etc/version"
if [ -f "${VERSION_FILE}" ]; then
    FW_VERSION=$(cat "${VERSION_FILE}" | tr -d '[:space:]')
else
    echo "❌ 错误: 未找到版本文件 ${VERSION_FILE}"
    exit 1
fi
echo "--> 固件版本号: ${FW_VERSION}"

# 1. 检查当前目录下是否存在 myconfig 文件
if [ ! -f "./myconfig" ]; then
    echo "❌ 错误: 当前目录下未找到 'myconfig' 文件，请检查输入！"
    exit 1
fi

# 步骤 1: 把当前目录下的 myconfig 文件，覆盖掉 ./output/.config
echo "--> 步骤 1: 正在将 myconfig 复制到 ./output/.config ..."
cp ./myconfig ./output/.config

# 步骤 2: 清理旧文件并生成新的精简版 defconfig
echo "--> 步骤 2: 正在清理可能存在的旧配置文件..."
# [新增逻辑] 先斩后奏：强制删除旧的 openipc_defconfig
rm -f ./output/openipc_defconfig

echo "--> 正在进入 output 目录生成全新的 openipc_defconfig ..."
cd ./output

# 执行生成指令
if [ -f "Makefile" ]; then
    make savedefconfig
else
    cd ..
    make BOARD=my savedefconfig
    cd ./output
fi

# [新增逻辑] 严格检查新文件是否真的生成成功了
echo "--> 正在校验新文件是否生成..."
if [ -f "./openipc_defconfig" ] && [ -s "./openipc_defconfig" ]; then
    echo "   ✓ 校验通过：新的 openipc_defconfig 已成功生成且不为空！"
else
    echo "❌ 错误: 全新的 openipc_defconfig 未能成功生成，流程中断！"
    exit 1
fi

# 步骤 3: 将 output 下生成的 openipc_defconfig 考回到 ./br-ext-chip-goke/configs，改名为 my_defconfig
echo "--> 步骤 3: 将新生成的精简配置移至板级目录并重命名为 my_defconfig ..."
mkdir -p ../br-ext-chip-goke/configs/
cp ./openipc_defconfig ../br-ext-chip-goke/configs/my_defconfig
cd ..

# 步骤 3.5: 修复所有文本文件的 CRLF 换行符为 LF
# 背景：Windows 提交的文件可能带 CRLF，设备端 ash/busybox 解析时会因 \r 报错
# （如 default.script 解析失败导致 udhcpc 拿不到 IP）。
# 此处对 general/(overlay+packages) 和 br-ext-chip-*/ 下的所有文本文件强制转 LF。
echo "--> 步骤 3.5: 检查并修复文本文件 CRLF 换行符..."
CRLF_DIRS="./general ./br-ext-chip-*"
CRLF_LIST=$(mktemp)
# -I 跳过二进制，-l 仅列出含 CR 的文件；2>/dev/null 抑制 "binary file" 提示
find $CRLF_DIRS -type f 2>/dev/null -exec grep -Il $'\r' {} + 2>/dev/null > "$CRLF_LIST" || true
CRLF_COUNT=$(wc -l < "$CRLF_LIST")
if [ "$CRLF_COUNT" -gt 0 ]; then
    echo "   发现 ${CRLF_COUNT} 个含 CRLF 的文本文件，转换为 LF..."
    xargs -r -a "$CRLF_LIST" sed -i 's/\r$//'
    echo "   ✓ 已修复 ${CRLF_COUNT} 个文件"
else
    echo "   ✓ 所有文本文件已是 LF 换行，无需修复"
fi
rm -f "$CRLF_LIST"

# 步骤 3.6: 修复被 Windows 破坏的符号链接
# 背景：Windows git 默认 core.symlinks=false，符号链接被转成普通文件（内容为目标路径）。
# 这些伪符号链接文件打包进固件后，调用方会把目标路径当成命令执行而报错
# （如 check_mac -> extutils 被破坏后报 syntax error）。
# 此处扫描所有小文件（<256字节），若内容是相对路径且目标存在，则重建为符号链接。
echo "--> 步骤 3.6: 检查并修复被破坏的符号链接..."
SYMLINK_COUNT=0
while read -r f; do
    # 读取文件内容（去除首尾空白），符号链接目标通常是简短相对路径
    target=$(head -c 256 "$f" 2>/dev/null | tr -d '[:space:]')
    [ -z "$target" ] && continue
    # 符号链接目标只含路径安全字符，不含换行
    case "$target" in
        *[!a-zA-Z0-9_./-]*) continue ;;
    esac
    # 目标必须存在且不是目录（避免误判）
    target_path="$(dirname "$f")/$target"
    [ -e "$target_path" ] || continue
    [ -d "$target_path" ] && continue
    # 重建符号链接
    rm -f "$f"
    ln -s "$target" "$f"
    SYMLINK_COUNT=$((SYMLINK_COUNT + 1))
    echo "   ✓ 修复符号链接: ${f#./} -> $target"
done < <(find ./general ./br-ext-chip-* -type f -size -256c 2>/dev/null)
echo "   ✓ 共修复 ${SYMLINK_COUNT} 个符号链接"

# 步骤 3.7: 确保 extutils 的多调用符号链接存在
# 背景：extutils 是多调用脚本（类似 busybox），通过 $0 判断调用名称。
# check_mac/cli/sysinfo/netip_hash 必须是指向 extutils 的符号链接。
# Windows 无法创建符号链接（需管理员/开发者模式），故在构建时（Linux）创建。
echo "--> 步骤 3.7: 确保 extutils 多调用符号链接..."
EXTUTILS_DIR="./general/overlay/usr/sbin"
if [ -f "$EXTUTILS_DIR/extutils" ]; then
    for link in check_mac cli sysinfo netip_hash; do
        if [ ! -L "$EXTUTILS_DIR/$link" ]; then
            rm -f "$EXTUTILS_DIR/$link"
            ln -s extutils "$EXTUTILS_DIR/$link"
            echo "   ✓ 创建符号链接: $link -> extutils"
        fi
    done
else
    echo "   ⚠ extutils 不存在，跳过符号链接创建"
fi

# 步骤 4: 调用 make BOARD=my clean all 编译（已在顶层目录）
echo "--> 步骤 4: 开始全量清洗并编译板型 [my] ..."

# 执行终极编译
make BOARD=my clean all

# 编译结束后，将版本号嵌入固件文件名
# 命名规则：真实扩展名(.tgz/.bin/.tar/.cpio/.img)保留在末尾，
#           板型后缀(.gk7205v300)视为文件名一部分，版本号追加到其后面
echo "--> 步骤 5: 将版本号嵌入固件文件名 ..."
append_version() {
    local src="$1" ver="$2"
    case "$src" in
        *.tgz|*.bin|*.tar|*.cpio|*.img)
            echo "${src%.*}-${ver}.${src##*.}"
            ;;
        *)
            echo "${src}-${ver}"
            ;;
    esac
}

VERSIONED_COUNT=0
for pat in "openipc.*.tgz" "uImage.*" "rootfs.squashfs.*"; do
    for f in ./output/images/$pat; do
        [ -f "$f" ] || continue
        fname=$(basename "$f")
        vname=$(append_version "$fname" "$FW_VERSION")
        mv -f "$f" "./output/images/$vname"
        echo "   ✓ 已重命名固件: $vname"
        VERSIONED_COUNT=$((VERSIONED_COUNT + 1))
    done
done

if [ "$VERSIONED_COUNT" -eq 0 ]; then
    echo "   ⚠️  未找到 output/images/ 下的固件产物（openipc.*.tgz / uImage.* / rootfs.squashfs.*），跳过版本重命名"
fi

echo "=================================================="
echo "🎉 恭喜！OpenIPC [my] 板型固件编译完成！"
echo "固件版本号: ${FW_VERSION}"
echo "固件产物存放在 ./output/images/ 目录下。"
echo "已生成带版本号(${FW_VERSION})的固件数量: ${VERSIONED_COUNT}"
echo "=================================================="
