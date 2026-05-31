#!/bin/bash
# -------------------------------------------------------------
# OpenIPC FactoryInit 自动化一键本地编译部署脚本 (V2.0)
# -------------------------------------------------------------

# 配置目标摄像头信息（保持你的产线默认配置）
IPC_IP="192.168.0.101"
IPC_USER="root"
IPC_PASS="12345"
TARGET_DIR="/mnt/mmcblk0p1/bin"
TARGET_NAME="ipc_server"

echo "=== [1/3] 正在进入 src 目录调用 Makefile.local 独立编译 ==="

# ⚡ 关键修复：显式指定读取本地独立编译配置文件 Makefile.local
make -f Makefile.local clean
if ! make -f Makefile.local; then
    echo "❌ 独立编译失败，请检查 src/Makefile.local 中的编译器路径！"
    exit 1
fi

# 检查 Ubuntu 本地是否安装了 sshpass 工具
if ! command -v sshpass &>/dev/null; then
    echo "提示: 本地未检测到 sshpass，正在尝试通过 apt 自动安装..."
    sudo apt-get update && sudo apt-get install -y sshpass
fi

# ⚡ 新增：清理本地旧的 SSH 主机密钥（解决主机标识变更问题）
echo -e "\n=== 清理本地旧的 SSH 主机密钥 ==="
ssh-keygen -f "${HOME}/.ssh/known_hosts" -R "${IPC_IP}" &>/dev/null
echo "✅ 已清理 ${IPC_IP} 旧的 SSH 主机密钥"

echo -e "\n=== [2/3] 正在通过网络拷贝到摄像头 ($IPC_IP) ==="

# 💡 安全防护：如果摄像头里老程序正在运行，先强行杀掉它，否则文件被锁会导致传输失败
# ⚡ 强化 SSH 参数：增加 -o PasswordAuthentication=yes 强制启用密码认证
sshpass -p "${IPC_PASS}" ssh -o StrictHostKeyChecking=no -o PasswordAuthentication=yes ${IPC_USER}@${IPC_IP} "killall -9 ${TARGET_NAME} 2>/dev/null"

# 使用 scp 将编译出来的二进制文件推送到目标的存储卡目录
# ⚡ 同样强化 scp 的 SSH 参数
sshpass -p "${IPC_PASS}" scp -O -o StrictHostKeyChecking=no -o PasswordAuthentication=yes ${TARGET_NAME} ${IPC_USER}@${IPC_IP}:${TARGET_DIR}/

if [ $? -eq 0 ]; then
    echo "👍 文件传输成功！"
else
    echo "❌ 传输失败，请检查摄像头网络、IP地址、密码是否正确！"
    exit 1
fi

echo -e "\n=== [3/3] 正在远程赋予执行权限并尝试后台唤醒 ==="
# 远程赋予可执行权限，杀掉旧的占位 socat，并直接在摄像头后台拉起你的全新专属服务
# ⚡ 强化 SSH 参数
sshpass -p "${IPC_PASS}" ssh -o StrictHostKeyChecking=no -o PasswordAuthentication=yes ${IPC_USER}@${IPC_IP} \
    "chmod +x ${TARGET_DIR}/${TARGET_NAME} && killall -9 socat 2>/dev/null; ${TARGET_DIR}/${TARGET_NAME} &"

echo -e "\n=========================================================="
echo "  [ALL DONE] 本地独立编译与远程部署完成！"
echo "  🚀 你的新产线工具已在摄像头后台挂起运行 (端口: 8086)"
echo "=========================================================="