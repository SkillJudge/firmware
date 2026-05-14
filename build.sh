#!/bin/bash

# 自动加载你的配置 + 编译
# 以后直接运行：./build.sh

# 1. 加载默认配置
make BOARD=gk7205v300_ultimate defconfig

# 2. 覆盖你自己保存好的完整配置（WiFi、squashfs、GPIO、I2C全部在）
cp br-ext-chip-goke/configs/gk7205v300_ultimate_defconfig_ecnu output/.config

# 3. 清理旧文件
make clean

# 4. 开始编译
make BOARD=gk7205v300_ultimate all

echo -e "\n====================================="
echo " 编译完成！固件在 output/images/ 目录下"
echo "=====================================\n"
