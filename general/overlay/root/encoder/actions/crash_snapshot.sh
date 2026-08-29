#!/bin/sh
# =============================================================
# crash_snapshot.sh — 进程死亡现场取证（encalertd 6103 确认时调用）
#
# 用法: crash_snapshot.sh '{"procs":"encoder_main,listener"}'
# 收集: dmesg tail + 启动/业务日志 + core dump + 系统快照
# 落盘: $CRASH_DIR/crash_<YYYYmmdd_HHMMSS>.tar（BusyBox tar 无 gzip，纯 tar）
#       滚动保留最近 KEEP 份
# 输出: {"snapshot":"crash_xxx.tar","dmesgKey":"...","saved":1}
#
# 业务日志清单：encodermain 定稿后如有新增日志路径，追加到 LOG_FILES。
# =============================================================
CRASH_DIR="${CRASH_DIR:-/mnt/mmcblk0p1/logs/crash}"
KEEP="${KEEP:-10}"
JQ=/root/resources/jq

PROCS=$( [ -x "$JQ" ] && "$JQ" -r '.procs // empty' <<EOF 2>/dev/null
$1
EOF
)
[ -n "$PROCS" ] || PROCS=$(printf '%s' "$1" | sed -n 's/.*"procs":"\([^"]*\)".*/\1/p')

mkdir -p "$CRASH_DIR" 2>/dev/null
WORK="/tmp/crash_snap.$$"
mkdir -p "$WORK"
TS=$(date +%Y%m%d_%H%M%S)

# 1. 内核日志（段错误/OOM/panic 定位第一现场）
dmesg 2>/dev/null | tail -300 > "$WORK/dmesg_tail.txt"
KEY=$(grep -iE 'segfault|oom|out of memory|panic|killed process|bad mode|bug:' \
      "$WORK/dmesg_tail.txt" 2>/dev/null | tail -3 | tr '\n' ';' | cut -c1-160)

# 2. 启动器与业务日志（存在才收）
for lf in /tmp/start_encoder.log /tmp/encalertd.log \
          /tmp/encoder_main.log /tmp/listener.log /tmp/heartbeat.log; do
    [ -f "$lf" ] && cp "$lf" "$WORK/$(basename $lf)" 2>/dev/null
done
# 与死亡进程同名的日志兜底（encodermain 定稿后的新日志自动被收集）
if [ -n "$PROCS" ]; then
    echo "$PROCS" | tr ',' '\n' | while IFS= read -r p; do
        [ -f "/tmp/$p.log" ] && cp "/tmp/$p.log" "$WORK/" 2>/dev/null
    done
fi

# 3. core dump（core_pattern=core，落在进程 cwd；扫常见目录）
for cd in /tmp /root /root/encoder /var/run /mnt/mmcblk0p1; do
    for cf in "$cd"/core*; do
        [ -f "$cf" ] || continue
        sz=$(stat -c '%s' "$cf" 2>/dev/null || echo 0)
        # 50MB 以上的 core 只记名不打包，防快照撑爆 SD
        if [ "$sz" -lt 52428800 ]; then
            cp "$cf" "$WORK/" 2>/dev/null
        else
            echo "skip big core: $cf ($sz bytes)" >> "$WORK/core_skipped.txt"
        fi
    done
done

# 4. 系统现场快照
{
    echo "=== procs dead: $PROCS"
    echo "=== date: $(date)"
    echo "=== uptime: $(cat /proc/uptime 2>/dev/null)"
    echo "=== ps:"
    ps 2>/dev/null
    echo "=== meminfo(head):"
    head -8 /proc/meminfo 2>/dev/null
    echo "=== df:"
    df -m 2>/dev/null
} > "$WORK/sysinfo.txt"

# 5. 打包 + 滚动保留
TARF="$CRASH_DIR/crash_${TS}.tar"
tar cf "$TARF" -C "$WORK" . 2>/dev/null
rm -rf "$WORK"
SAVED=0
if [ -f "$TARF" ]; then
    SAVED=1
    ls -1t "$CRASH_DIR"/crash_*.tar 2>/dev/null | tail -n +$((KEEP + 1)) | \
        while IFS= read -r old; do rm -f "$old"; done
fi

printf '{"snapshot":"crash_%s.tar","dmesgKey":"%s","saved":%d}' \
       "$TS" "$KEY" "$SAVED"
exit 0
