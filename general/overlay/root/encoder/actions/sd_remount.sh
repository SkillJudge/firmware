#!/bin/sh
#
# sd_remount.sh — 尝试把 mmcblk0p1 挂回 ${SDCARD_MOUNT_POINT}
# 由 encalertd 调用（契约见 EncoderAlertdDesign.md §3.2）：
#   argv[1] = context JSON（本脚本不使用）
#   输出    = 单行 JSON；exit 0=成功
PATH=/usr/sbin:/usr/bin:/sbin:/bin

MP="${SDCARD_MOUNT_POINT:-/mnt/mmcblk0p1}"
DEV="/dev/mmcblk0p1"

is_mounted() {
    grep -q " $MP " /proc/mounts 2>/dev/null
}

mount $DEV "$MP" >/dev/null 2>&1 || \
mount -t vfat $DEV "$MP" >/dev/null 2>&1 || \
mount -t ext4 $DEV "$MP" >/dev/null 2>&1

sleep 1

if is_mounted; then
    echo '{"rc":0,"note":"remounted"}'
    exit 0
fi
echo '{"rc":1,"note":"remount_failed"}'
exit 1
