#!/bin/sh
# =====================================================================
# overlay_log_mgr.sh — OpenIPC 编码器 overlay 分区日志管理器
#
# 功能：
#   1. 监控 /overlay (rootfs_data) 分区使用率
#   2. 根据三级水位（WARN/HIGH/CRIT）执行分级清理：
#        - WARN：仅删除历史轮转文件（*.1、*.old、*.bak、*.gz）
#        - HIGH：额外截断仍在增长的大日志（保留尾部 64KB/最近内容）
#        - CRIT：紧急清理 tmp/ 目录中超过 24h 的临时文件
#   3. 绝对不碰 encodermain 的状态文件（state/、pidfile、lock）
#   4. 不依赖任何 encodermain / factoryinit / encalertd 接口
#
# 用法：
#   /usr/sbin/overlay_log_mgr.sh             # 单次清理（默认，cron 调用）
#   /usr/sbin/overlay_log_mgr.sh --status    # 只打印状态，不做清理
#   /usr/sbin/overlay_log_mgr.sh --force     # 强制执行 HIGH 级别清理（忽略水位）
#   /usr/sbin/overlay_log_mgr.sh --daemon N  # 前台模式，每 N 秒执行一次（不推荐，优先 cron）
#
# 部署：
#   通过 crond 每 15 分钟调用一次（cron 片段见同脚本 install_cron()）
# =====================================================================
PATH=/usr/sbin:/usr/bin:/sbin:/bin
SCRIPT_VERSION="1.0.0"

# ---------- 可调阈值（环境变量可覆盖） ----------
WARN_USE_PCT="${OVERLAY_WARN_PCT:-75}"    # 达到该值仅清理旧轮转
HIGH_USE_PCT="${OVERLAY_HIGH_PCT:-85}"   # 达到该值开始截断活跃日志
CRIT_USE_PCT="${OVERLAY_CRIT_PCT:-93}"   # 达到该值紧急清理 tmp

ACTIVE_LOG_KEEP_BYTES="${OVERLAY_LOG_KEEP_BYTES:-65536}"  # 活跃日志保留尾部字节数 (64KB)
CRIT_TMP_MAX_AGE_HOURS="${OVERLAY_CRIT_TMP_AGEH:-24}"    # CRIT 模式 tmp 文件保留小时数

# ---------- 安全白名单（只允许操作以下路径） ----------
# 注意：状态目录 state/、pidfile、lock 文件均不在白名单中
LOG_TARGETS="
/root/encoder/runtime/logs
/var/log
"
CRIT_TMP_TARGETS="/root/encoder/runtime/tmp"

# ---------- 日志函数（自身日志使用 /tmp，避免进 overlay 循环占空间） ----------
LOG_TAG="overlay_log_mgr"
loginfo()  { logger -t "$LOG_TAG" -s "INFO: $*" 2>/dev/null || echo "[$(date '+%H:%M:%S') INFO] $*"; }
logwarn()  { logger -t "$LOG_TAG" -s "WARN: $*" 2>/dev/null || echo "[$(date '+%H:%M:%S') WARN] $*" >&2; }
logerr()   { logger -t "$LOG_TAG" -s "ERR : $*" 2>/dev/null || echo "[$(date '+%H:%M:%S') ERR ] $*" >&2; }

# ---------- 获取当前 overlay 使用率（整数百分比） ----------
get_overlay_use_pct() {
    # /overlay 在 pivot_root 后存在；若不存在（非常早期启动），安全返回 0
    if ! mountpoint -q /overlay 2>/dev/null && [ ! -d /overlay/root ]; then
        echo 0
        return 0
    fi
    local line
    line=$(df -k -P /overlay 2>/dev/null | awk 'NR==2 {print $5}' | tr -d '%')
    case "$line" in
        ''|*[!0-9]*) echo 0 ;;
        *) echo "$line" ;;
    esac
}

# ---------- 文件是否仍被活跃进程持有（通过 /proc/*/fd 判断） ----------
is_file_open() {
    local f="$1"
    [ -f "$f" ] || return 1
    local fd
    for fd in /proc/*/fd/*; do
        # /proc/PID/fd/N 可能是 /proc/self/fd，跳过无效链接
        local tgt
        tgt=$(readlink "$fd" 2>/dev/null) || continue
        [ "$tgt" = "$f" ] && return 0
    done
    return 1
}

# ---------- 级别 1：删除所有旧轮转/备份文件（*.1 *.old *.bak *.gz *.tgz） ----------
purge_rotated() {
    local total_bytes=0 removed=0
    local root
    for root in $LOG_TARGETS; do
        [ -d "$root" ] || continue
        # 用 find 严格按扩展名匹配，避免匹配 state/ 中的任何内容
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            # 双重保险：父目录必须属于我们的 LOG_TARGETS 之一
            local ok=0 p
            for p in $LOG_TARGETS; do
                case "$f" in "$p"/*) ok=1; break ;; esac
            done
            [ "$ok" -eq 1 ] || continue
            local sz
            sz=$(stat -c '%s' "$f" 2>/dev/null || echo 0)
            case "$sz" in ''|*[!0-9]*) sz=0 ;; esac
            if rm -f "$f" 2>/dev/null; then
                total_bytes=$(( total_bytes + sz ))
                removed=$(( removed + 1 ))
            fi
        done <<EOF
$(find $LOG_TARGETS -type f \
    \( -name '*.1' -o -name '*.2' -o -name '*.3' -o -name '*.4' -o -name '*.5' \
       -o -name '*.old' -o -name '*.bak' -o -name '*.swp' \
       -o -name '*.gz' -o -name '*.tgz' -o -name '*.bz2' -o -name '*.xz' \
       -o -name '*.log.*' \) \
    2>/dev/null)
EOF
    done
    loginfo "purge_rotated: removed=$removed freed_kb=$(( total_bytes / 1024 ))"
}

# ---------- 级别 2：截断活跃大日志（保留尾部 $ACTIVE_LOG_KEEP_BYTES 字节，原地改 inode 不变） ----------
trim_active_logs() {
    local root f keep_bytes="$ACTIVE_LOG_KEEP_BYTES"
    local trimmed=0 freed_bytes=0
    # 截断阈值：大于 2x 保留大小才动手，避免无意义 IO
    local min_trim_sz=$(( keep_bytes * 2 ))

    for root in $LOG_TARGETS; do
        [ -d "$root" ] || continue
        # 仅匹配 *.log 和 encodermain / encalertd / majestic 的默认日志名
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            [ -f "$f" ] || continue
            local sz
            sz=$(stat -c '%s' "$f" 2>/dev/null || echo 0)
            case "$sz" in ''|*[!0-9]*) continue ;; esac
            [ "$sz" -lt "$min_trim_sz" ] && continue

            local tmpf="/tmp/.olgm_$$_trim"
            # 保留尾部 -> tmp，再写回原文件（inode 不变，已打开的 fd 不会断）
            if tail -c "$keep_bytes" "$f" > "$tmpf" 2>/dev/null; then
                local trimmed_sz
                trimmed_sz=$(stat -c '%s' "$tmpf" 2>/dev/null || echo 0)
                if cat "$tmpf" > "$f" 2>/dev/null; then
                    local diff=$(( sz - trimmed_sz ))
                    [ "$diff" -gt 0 ] && freed_bytes=$(( freed_bytes + diff ))
                    trimmed=$(( trimmed + 1 ))
                fi
            fi
            rm -f "$tmpf"
        done <<EOF
$(find "$root" -maxdepth 3 -type f \
    \( -name '*.log' -o -name 'encodermain*' -o -name 'encalertd*' -o -name 'messages' \
       -o -name 'syslog' -o -name 'klog' -o -name 'majestic*' \) \
    2>/dev/null)
EOF
    done
    sync
    loginfo "trim_active_logs: trimmed=$trimmed freed_kb=$(( freed_bytes / 1024 ))"
}

# ---------- 级别 3：CRIT 模式清理 tmp 目录的陈旧临时文件 ----------
purge_stale_tmp() {
    local target removed=0 freed_bytes=0
    for target in $CRIT_TMP_TARGETS; do
        [ -d "$target" ] || continue
        # tmp 目录内任何内容均可删（encodermain 启动时会重建临时目录）
        # 限制：只删 mtime 超过 N 小时的文件，避免误伤刚写的临时文件
        local min_age_min=$(( CRIT_TMP_MAX_AGE_HOURS * 60 ))
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            [ -f "$f" ] || continue
            local sz
            sz=$(stat -c '%s' "$f" 2>/dev/null || echo 0)
            case "$sz" in ''|*[!0-9]*) sz=0 ;; esac
            if rm -f "$f" 2>/dev/null; then
                freed_bytes=$(( freed_bytes + sz ))
                removed=$(( removed + 1 ))
            fi
        done <<EOF
$(find "$target" -type f -mmin +"$min_age_min" 2>/dev/null)
EOF
    done
    # 清理空目录但不删 tmp 根本身
    for target in $CRIT_TMP_TARGETS; do
        [ -d "$target" ] && find "$target" -depth -type d -empty -exec rmdir {} \; 2>/dev/null || true
    done
    sync
    loginfo "purge_stale_tmp: removed=$removed freed_kb=$(( freed_bytes / 1024 ))"
}

# ---------- 打印状态（--status） ----------
print_status() {
    local use_pct mountp overlay_size_kb used_kb free_kb
    use_pct=$(get_overlay_use_pct)
    if df -k -P /overlay >/dev/null 2>&1; then
        read -r _ overlay_size_kb used_kb free_kb _ mountp <<EOF
$(df -k -P /overlay | awk 'NR==2')
EOF
    else
        overlay_size_kb="?" used_kb="?" free_kb="?" mountp="?"
    fi

    echo "================================================================"
    echo " overlay_log_mgr v$SCRIPT_VERSION  Status Report"
    echo "================================================================"
    echo " Overlay mount : $mountp"
    echo " Total         : $overlay_size_kb KB"
    echo " Used          : $used_kb KB"
    echo " Free          : $free_kb KB"
    echo " Usage         : ${use_pct}%"
    echo " Thresholds    : WARN=${WARN_USE_PCT}%  HIGH=${HIGH_USE_PCT}%  CRIT=${CRIT_USE_PCT}%"
    echo " Log roots     : $LOG_TARGETS"
    echo " Crit tmp dirs : $CRIT_TMP_TARGETS"
    echo "----------------------------------------------------------------"
    # 各日志根目录占用 top 10
    local r
    for r in $LOG_TARGETS; do
        if [ -d "$r" ]; then
            echo ""
            echo " [Top files under $r]"
            du -k "$r" 2>/dev/null | sort -rn | head -n 10 | awk '{printf "   %8d KB  %s\n", $1, $2}'
        fi
    done
    echo "================================================================"
}

# ---------- 单次执行主流程（cron 模式） ----------
run_once() {
    local force="$1"
    local use_pct
    use_pct=$(get_overlay_use_pct)
    loginfo "overlay usage=${use_pct}% (W=${WARN_USE_PCT} H=${HIGH_USE_PCT} C=${CRIT_USE_PCT})"

    local level="NONE"
    if [ "$force" = "1" ]; then
        level="HIGH"
    elif [ "$use_pct" -ge "$CRIT_USE_PCT" ]; then
        level="CRIT"
    elif [ "$use_pct" -ge "$HIGH_USE_PCT" ]; then
        level="HIGH"
    elif [ "$use_pct" -ge "$WARN_USE_PCT" ]; then
        level="WARN"
    fi

    case "$level" in
        NONE)
            loginfo "level=NONE, no action required."
            ;;
        WARN)
            loginfo "level=WARN, purge rotated backups only."
            purge_rotated
            ;;
        HIGH)
            loginfo "level=HIGH, purge rotated + trim active logs."
            purge_rotated
            trim_active_logs
            ;;
        CRIT)
            loginfo "level=CRIT, purge rotated + trim active + stale tmp."
            purge_rotated
            trim_active_logs
            purge_stale_tmp
            ;;
    esac

    # 记录执行后的 overlay 水位
    local after
    after=$(get_overlay_use_pct)
    loginfo "after cleanup overlay usage=${after}%"
}

# ---------- 安装 cron 片段（由 --install 触发或首次部署手动调用） ----------
# 注意：本板子 busybox crond 以 "-c /etc/crontabs" 启动，crontab 放在该目录下按用户名命名。
install_cron() {
    local cron_root="/etc/crontabs"
    local cron_file="$cron_root/root"
    mkdir -p "$cron_root" 2>/dev/null
    local MARK="### overlay_log_mgr-cron-begin ###"
    local END="### overlay_log_mgr-cron-end ###"
    # 先移除旧条目（如果存在）再追加，保证幂等
    if grep -q "$MARK" "$cron_file" 2>/dev/null; then
        local tmp="/tmp/.olgm_cron_$$"
        awk -v m="$MARK" -v e="$END" '
            $0==m {skip=1; next}
            $0==e {skip=0; next}
            {if (!skip) print}
        ' "$cron_file" > "$tmp" 2>/dev/null
        mv "$tmp" "$cron_file" 2>/dev/null
    fi
    # 追加条目（busybox crontabs 文件不需要用户名字段）
    {
        echo ""
        echo "$MARK"
        echo "# 每 15 分钟检查 overlay 分区水位并分级清理日志"
        echo "*/15 * * * * /usr/sbin/overlay_log_mgr.sh >/dev/null 2>&1"
        echo "$END"
    } >> "$cron_file"
    chmod 600 "$cron_file"
    loginfo "cron entry installed/updated: $cron_file"
    echo "Installed/updated cron entry in $cron_file"
    # busybox crond 自动重读；保险起见重启 crond（不影响业务）
    if [ -x /etc/init.d/S60crond ]; then
        /etc/init.d/S60crond restart >/dev/null 2>&1 || true
    else
        killall -HUP crond 2>/dev/null || true
    fi
}

# ---------- 参数分发 ----------
main() {
    local force=0
    while [ $# -gt 0 ]; do
        case "$1" in
            -s|--status)    print_status; exit 0 ;;
            -f|--force)     force=1; shift ;;
            -i|--install)   install_cron; exit 0 ;;
            -d|--daemon)
                shift
                local interval="${1:-900}"  # 默认 15 分钟
                case "$interval" in ''|*[!0-9]*) interval=900 ;; esac
                loginfo "daemon mode: every ${interval}s (use cron instead if possible)"
                while true; do
                    run_once 0
                    sleep "$interval"
                done
                ;;
            -h|--help)
                cat <<'HELP'
Usage: overlay_log_mgr.sh [OPTIONS]

Options:
  (default)      Single pass: check overlay usage and cleanup per thresholds.
  -s, --status   Print usage/status report only (no changes).
  -f, --force    Force HIGH-level cleanup regardless of usage.
  -i, --install  Install cron job (/etc/cron.d/overlay_log_mgr, every 15 min).
  -d, --daemon N Run in foreground, loop every N seconds (default 900).
  -h, --help     Show this help.

Environment overrides:
  OVERLAY_WARN_PCT (default 75)     Warning threshold (%)
  OVERLAY_HIGH_PCT (default 85)     Start trimming active logs (%)
  OVERLAY_CRIT_PCT (default 93)     Emergency: purge stale tmp files (%)
  OVERLAY_LOG_KEEP_BYTES (65536)    Tail bytes kept when trimming active logs
  OVERLAY_CRIT_TMP_AGEH (24)        Max age (hours) for tmp files in CRIT mode
HELP
                exit 0
                ;;
            *)
                echo "Unknown option: $1" >&2
                exit 2
                ;;
        esac
    done
    run_once "$force"
}

main "$@"
exit 0
