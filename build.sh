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

# 步骤 4: 回到顶层目录，调用 make BOARD=my clean all 编译
echo "--> 步骤 4: 返回根目录，开始全量清洗并编译板型 [my] ..."
cd ..

# 执行终极编译
make BOARD=my clean all

# 编译结束后，将版本号嵌入固件文件名
echo "--> 步骤 5: 将版本号嵌入固件文件名 ..."
ORIG_IMG=$(ls -t ./output/images/openipc-*.bin 2>/dev/null | head -1)
if [ -n "${ORIG_IMG}" ] && [ -f "${ORIG_IMG}" ]; then
    IMG_BASENAME=$(basename "${ORIG_IMG}")
    IMG_NAME="${IMG_BASENAME%.bin}"
    VERSIONED_IMG="./output/images/${IMG_NAME}-${FW_VERSION}.bin"
    cp -f "${ORIG_IMG}" "${VERSIONED_IMG}"
    echo "   ✓ 已生成带版本号的固件: ${VERSIONED_IMG}"
else
    echo "   ⚠️  未找到 output/images/openipc-*.bin，跳过版本重命名"
fi

echo "=================================================="
echo "🎉 恭喜！OpenIPC [my] 板型固件编译完成！"
echo "固件版本号: ${FW_VERSION}"
echo "固件产物存放在 ./output/images/ 目录下。"
echo "带版本号的固件: ${VERSIONED_IMG:-N/A}"
echo "=================================================="
