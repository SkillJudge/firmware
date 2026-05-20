#!/bin/bash

# ===================== 配置区域 =====================
# 你的备份配置路径
MY_CONFIG="../firmware/myconfig"
# 目标配置路径（output/.config）
TARGET_CONFIG="../firmware/output/.config"
# 编译命令（你原本的命令）
BUILD_CMD="make BOARD=gk7205v300_ultimate clean all"
# ====================================================

echo "================================================"
echo "          OpenIPC 自动编译脚本"
echo "================================================"
echo ""

# 检查备份配置是否存在
if [ ! -f "$MY_CONFIG" ]; then
    echo "❌ 错误：备份配置 $MY_CONFIG 不存在！"
    exit 1
fi

# 复制配置文件（覆盖）
echo "✅ 正在复制配置文件：$MY_CONFIG -> $TARGET_CONFIG"
cp -f "$MY_CONFIG" "$TARGET_CONFIG"

# 检查复制是否成功
if [ ! -f "$TARGET_CONFIG" ]; then
    echo "❌ 错误：配置文件复制失败！"
    exit 1
fi

echo ""
echo "🚀 开始编译：$BUILD_CMD"
echo ""

# 执行编译
eval $BUILD_CMD

# 检查编译结果
if [ $? -eq 0 ]; then
    echo ""
    echo "================================================"
    echo "✅ 编译完成！"
    echo "================================================"
else
    echo ""
    echo "================================================"
    echo "❌ 编译失败！"
    echo "================================================"
    exit 1
fi
