#!/bin/bash
# -------------------------------------------------------------
# OpenIPC FactoryInit 自动化一键编译部署脚本
# -------------------------------------------------------------

# 配置目标摄像头信息
IPC_IP="192.168.0.101"
IPC_USER="root"
IPC_PASS="12345"
TARGET_DIR="/mnt/mmcblk0p1/bin"
TARGET_NAME="ipc_server"

echo "=== [1/3] 正在本地调用 Makefile 编译程序 ==="
make clean
if ! make; then
    echo "❌ 编译失败，请检查代码或编译器路径！"
    exit 1
fi

# 检查 Ubuntu 本地是否安装了 sshpass 工具（用于自动填密码）
if ! command -v sshpass &> /dev/null; then
    echo "提示: 本地未检测到 sshpass，正在尝试通过 apt 自动安装..."
    sudo apt-get update && sudo apt-get install -y sshpass
fi

echo -e "\n=== [2/3] 正在通过网络拷贝到摄像头 ($IPC_IP) ==="
# 使用 scp 强行推送到目标的指定目录
sshpass -p "${IPC_PASS}" scp -O -o StrictHostKeyChecking=no ${TARGET_NAME} ${IPC_USER}@${IPC_IP}:${TARGET_DIR}/

if [ $? -eq 0 ]; then
    echo "👍 文件传输成功！"
else
    echo "❌ 传输失败，请检查摄像头网络、IP地址、密码是否正确！"
    exit 1
fi

echo -e "\n=== [3/3] 正在远程赋予执行权限并清理残留 ==="
# 远程执行 chmod，并顺便帮你在摄像头里把老旧的 socat 杀掉
sshpass -p "${IPC_PASS}" ssh -o StrictHostKeyChecking=no ${IPC_USER}@${IPC_IP} "chmod +x ${TARGET_DIR}/${TARGET_NAME} && killall -9 socat 2>/dev/null; echo '🎉 摄像头端准备就绪！'"

echo -e "\n=========================================================="
echo "  [ALL DONE] 部署完成！"
echo "  你可以在摄像头终端直接运行: ${TARGET_DIR}/${TARGET_NAME}"
echo "=========================================================="
