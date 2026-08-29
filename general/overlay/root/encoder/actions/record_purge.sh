#!/bin/sh
#
# record_purge.sh — 录像清理（双模式，由 encalertd 调用）
#
# 用法：/bin/sh record_purge.sh '<json_ctx>'
#   时间模式 ctx: {"olderThanHours":24}   删除 mtime 早于 N 小时的录像
#   空间模式 ctx: {"minFreeMb":5120}      按 mtime 最旧优先删除，直至空闲 >= N MB
#
# 环境变量覆盖（测试/特殊部署用）：
#   RECORD_PURGE_ROOT          清理根目录（默认 /mnt/mmcblk0p1）
#   RECORD_PURGE_HOURS         ctx 未带 olderThanHours 时的默认值
#   RECORD_PURGE_MIN_FREE_MB   ctx 未带 minFreeMb 时的默认值
#
# 输出 = 单行 JSON {"mode":"time|space","purgedFiles":N,"freedBytes":B,"freeMb":M}
PATH=/usr/sbin:/usr/bin:/sbin:/bin

PURGE_ROOT="${RECORD_PURGE_ROOT:-/mnt/mmcblk0p1}"

[ -d "$PURGE_ROOT" ] || {
    echo '{"mode":"none","purgedFiles":0,"freedBytes":0,"freeMb":-1}'
    exit 0
}

ctx="$1"

# 从 ctx JSON 提取数字键（受控输入，仅 C 侧构造）
extract_num() {
    printf '%s' "$ctx" | grep -o "\"$1\":[0-9-]*" | head -n 1 | cut -d: -f2
}

HOURS=$(extract_num olderThanHours)
MINMB=$(extract_num minFreeMb)
[ -n "$HOURS" ] || HOURS="${RECORD_PURGE_HOURS:-24}"
[ -n "$MINMB" ] || MINMB="${RECORD_PURGE_MIN_FREE_MB:-0}"

free_mb() {
    df -k -P "$PURGE_ROOT" 2>/dev/null | awk 'NR==2{print int($4/1024)}'
}

deleted_count=0
freed_bytes=0
mode="time"

if [ "$MINMB" -gt 0 ]; then
    # ---------- 空间模式：最旧优先删除直至恢复保留水位 ----------
    mode="space"
    LIST="/tmp/purge_space.$$"
    # 候选 = 录像文件，按 mtime 升序（最旧在前）；分隔符 | 假定不出现在文件名
    find "$PURGE_ROOT" -type f \
        \( -name '*.mp4' -o -name '*.jpg' -o -name '*.jpeg' -o -name '*.ts' \) \
        2>/dev/null | while IFS= read -r f; do
            ts=$(stat -c '%Y' "$f" 2>/dev/null) || continue
            printf '%s|%s\n' "$ts" "$f"
        done | sort -t'|' -k1,1n > "$LIST"

    while IFS= read -r row; do
        f=${row#*|}
        [ -n "$f" ] || continue
        cur=$(free_mb)
        [ -n "$cur" ] && [ "$cur" -ge "$MINMB" ] && break
        sz=$(stat -c '%s' "$f" 2>/dev/null || echo 0)
        case "$sz" in ''|*[!0-9]*) sz=0 ;; esac
        rm -f "$f" 2>/dev/null || continue
        deleted_count=$(( deleted_count + 1 ))
        freed_bytes=$(( freed_bytes + sz ))
        # 单轮删除上限保护：防止海量小文件拖垮 30s action 超时
        [ "$deleted_count" -ge 500 ] && break
    done < "$LIST"
    rm -f "$LIST"
else
    # ---------- 时间模式：删除 mtime 早于 N 小时的录像 ----------
    tmp_list="/tmp/purge_list.$$"

    find "$PURGE_ROOT" -type f \
        \( -name '*.mp4' -o -name '*.jpg' -o -name '*.jpeg' -o -name '*.ts' \) \
        -mmin +$(( HOURS * 60 )) -print0 > "$tmp_list" 2>/dev/null || true

    while IFS= read -r -d '' f; do
        [ -n "$f" ] || continue
        fsize=$(stat -c '%s' "$f" 2>/dev/null || wc -c < "$f" 2>/dev/null || echo 0)
        case "$fsize" in ''|*[!0-9]*) fsize=0 ;; esac
        rm -f "$f" 2>/dev/null && {
            deleted_count=$(( deleted_count + 1 ))
            freed_bytes=$(( freed_bytes + fsize ))
        }
    done < "$tmp_list"
    rm -f "$tmp_list"
fi

end_free=$(free_mb)
[ -n "$end_free" ] || end_free=-1
echo "{\"mode\":\"${mode}\",\"purgedFiles\":${deleted_count},\"freedBytes\":${freed_bytes},\"freeMb\":${end_free}}"
exit 0
