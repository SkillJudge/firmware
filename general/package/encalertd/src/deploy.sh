#!/bin/bash
# -------------------------------------------------------------
# encalertd 一键独立编译 + 快速部署到编码器测试 (V1.0)
#
# 模式 3 —— 不烧固件，直接推到设备测试：
#   1. 本地调用 Makefile.local 交叉编译出 ARM 二进制
#   2. 推送 二进制 + 配置 + actions 脚本 到 SD 卡目录
#   3. 杀掉旧进程并后台拉起新进程
#
# 固化进固件请走 buildroot 大编译模式（模式 1），见包内 encalertd.mk。
#
# 目标编码器信息可通过环境变量覆盖：
#   ENC_IP / ENC_USER / ENC_PASS  ./deploy.sh
# -------------------------------------------------------------
set -u

ENC_IP="${ENC_IP:-192.168.250.101}"      # 编码器 WiFi 网段按现场修改
ENC_USER="${ENC_USER:-root}"
ENC_PASS="${ENC_PASS:-12345}"
REMOTE_BIN="/mnt/mmcblk0p1/bin"

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_DIR="$(dirname "$SRC_DIR")"                      # .../encalertd
GEN_DIR="$(dirname "$(dirname "$PKG_DIR")")"          # .../general
OVERLAY_DIR="${GEN_DIR}/overlay"
LOCAL_CONF="${OVERLAY_DIR}/etc/encalertd.conf"
LOCAL_ACTIONS="${OVERLAY_DIR}/root/encoder/actions"

echo "=== [1/3] 调用 Makefile.local 独立交叉编译 ==="
cd "${SRC_DIR}"
make -f Makefile.local clean
if ! make -f Makefile.local; then
    echo "[FAIL] 编译失败，检查 output/host 工具链是否已生成"
    exit 1
fi

if ! command -v sshpass >/dev/null 2>&1; then
    echo "[INFO] 未安装 sshpass，尝试 apt 安装..."
    sudo apt-get update && sudo apt-get install -y sshpass
fi

echo ""
echo "=== 清理本地旧的 SSH 主机密钥 ==="
ssh-keygen -f "${HOME}/.ssh/known_hosts" -R "${ENC_IP}" >/dev/null 2>&1 || true

SSH_OPTS="-o StrictHostKeyChecking=no -o PasswordAuthentication=yes -o ConnectTimeout=8"

echo ""
echo "=== [2/3] 推送二进制 / 配置 / actions 脚本 → ${ENC_USER}@${ENC_IP}:${REMOTE_BIN} ==="
# 先杀旧进程避免文件占用
sshpass -p "${ENC_PASS}" ssh ${SSH_OPTS} ${ENC_USER}@${ENC_IP} \
    "killall -9 encalertd 2>/dev/null; mkdir -p ${REMOTE_BIN}/actions" \
    && echo "[OK] 远程目录就绪"

scp_push() {
    sshpass -p "${ENC_PASS}" scp -O ${SSH_OPTS} "$@" || return 1
}

scp_push ./encalertd                                   "${ENC_USER}@${ENC_IP}:${REMOTE_BIN}/"        || { echo "[FAIL] 二进制推送失败"; exit 1; }
scp_push "${LOCAL_CONF}"                               "${ENC_USER}@${ENC_IP}:${REMOTE_BIN}/"        || { echo "[FAIL] 配置推送失败";   exit 1; }
scp_push "${LOCAL_ACTIONS}"/*.sh                       "${ENC_USER}@${ENC_IP}:${REMOTE_BIN}/actions/" || { echo "[FAIL] actions 推送失败"; exit 1; }

echo ""
echo "=== [3/3] 赋权、生成本地化配置并以独立模式启动 ==="
sshpass -p "${ENC_PASS}" ssh ${SSH_OPTS} ${ENC_USER}@${ENC_IP} "
    chmod +x ${REMOTE_BIN}/encalertd ${REMOTE_BIN}/actions/*.sh;
    mkdir -p ${REMOTE_BIN}/spool;
    cfg=${REMOTE_BIN}/encalertd.conf;
    sed -i '/^actions_dir=/d;/^spool_dir=/d' \$cfg;
    printf 'actions_dir=%s/actions\nspool_dir=%s/spool\n' \"${REMOTE_BIN}\" \"${REMOTE_BIN}\" >> \$cfg;
    rm -f /var/run/encalertd.pid;
    nohup sh -c \"${REMOTE_BIN}/encalertd -c \$cfg -b\" >/dev/null 2>&1 &
    sleep 2;
    if pidof encalertd >/dev/null; then
        echo '[OK] encalertd 已在设备上运行 pid='\$(pidof encalertd);
        echo '日志: tail -f /tmp/encalertd.log';
    else
        echo '[FAIL] 启动失败';
        ${REMOTE_BIN}/encalertd -t -c \$cfg 2>&1 | head -40;
    fi"

echo ""
echo "=============================================="
echo "  部署完成。自检模式: ${REMOTE_BIN}/encalertd -t"
echo "  停止:           killall encalertd"
echo "=============================================="
