#!/bin/sh
# =============================================================
# proc_restart.sh — 死亡进程拉起（encalertd 6103 确认时调用）
#
# 用法: proc_restart.sh '{"procs":"encoder_main,listener"}'
# 策略: 只拉"根进程"（可独立重启的入口）；
#       listener/heartbeat 等子进程由 encoder_main 家族内部管理，
#       单独拉起会破坏家族关系 → 只随根进程一起重启。
# 映射:
#   majestic      -> /etc/init.d/S95majestic start
#   encoder_main  -> /etc/init.d/S99zzencoder start（家族根，含 listener/heartbeat）
#   listener/heartbeat -> 不单独拉起（skip，随 encoder_main）
#   ipc_server (factoryinit 服务) -> /etc/init.d/S99factoryinit start
# 输出: {"restarted":"encoder_main","skipped":"listener,heartbeat"}
# =============================================================
JQ=/root/resources/jq

PROCS=$( [ -x "$JQ" ] && "$JQ" -r '.procs // empty' <<EOF 2>/dev/null
$1
EOF
)
[ -n "$PROCS" ] || PROCS=$(printf '%s' "$1" | sed -n 's/.*"procs":"\([^"]*\)".*/\1/p')

STATE_DIR="${STATE_DIR:-/root/encoder/runtime/state}"

echo "$PROCS" | tr ',' '\n' | while IFS= read -r p; do
    [ -n "$p" ] || continue
    case "$p" in
        majestic)
            /etc/init.d/S95majestic start >/dev/null 2>&1
            echo "majestic"
            ;;
        encoder_main)
            # 清理残留 pid 文件，避免启动窗口期旧 pid 再次误判
            rm -f "$STATE_DIR/encoder_main.pid"
            /etc/init.d/S99zzencoder start >/dev/null 2>&1
            echo "encoder_main"
            ;;
        listener|heartbeat)
            : # 子进程不单独拉起
            ;;
        ipc_server|factoryinit)
            /etc/init.d/S99factoryinit start >/dev/null 2>&1
            echo "ipc_server"
            ;;
        *)
            : # 未知进程不做动作（防误拉）
            ;;
    esac
done | {
    R=""; S=""
    while IFS= read -r line; do
        case "$line" in
            majestic|encoder_main|ipc_server) R="${R:+$R,}$line" ;;
            *) ;;
        esac
    done
    printf '{"restarted":"%s"}' "$R"
}
exit 0
