#!/bin/sh

# 登录 Shell 自动加载本脚本。
# 所有操作放在子 Shell 中执行，避免切换目录或退出动作影响用户当前终端。
(
    echo "[encoder] prepare executable permissions"
    sh /etc/profile.d/add_exec.sh --run || {
        echo "[encoder] add_exec failed, encoder will not start"
        exit 0
    }

    if [ ! -f /mnt/mmcblk0p1/jq-linux-armhf ]; then
        echo "[encoder] jq missing: /mnt/mmcblk0p1/jq-linux-armhf"
        exit 0
    fi

    # DEVICE_ID 暂时只提示，不在登录脚本中阻止启动尝试。
    if [ -z "${DEVICE_ID:-}" ]; then
        echo "[encoder] warning: DEVICE_ID is missing"
    else
        echo "[encoder] DEVICE_ID detected: $DEVICE_ID"
    fi

    echo "[encoder] schedule delayed encoder startup"
    nohup sh /etc/profile.d/start_encoder.sh --run > /tmp/start_encoder.log 2>&1 &
)
