#!/bin/sh
#
# stream_hup_majestic.sh — 推流链路自愈：向 majestic 发 HUP 重建 RTMP 连接
# 由 encalertd 的 stream_dead 确认回调调用。
#   输出 = 单行 JSON；exit 0=已发送 HUP 且进程仍在
PATH=/usr/sbin:/usr/bin:/sbin:/bin

majestic_alive() {
    [ -f /var/run/majestic.pid ] && kill -0 "$(cat /var/run/majestic.pid 2>/dev/null)" 2>/dev/null
}

if ! majestic_alive; then
    echo '{"rc":1,"note":"majestic_not_running_before_hup"}'
    exit 1
fi

killall -HUP majestic >/dev/null 2>&1

sleep 2

if majestic_alive; then
    echo '{"rc":0,"note":"hup_sent"}'
    exit 0
fi
echo '{"rc":1,"note":"majestic_gone_after_hup"}'
exit 1
