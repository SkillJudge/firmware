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
#   encodermain   -> /etc/init.d/S96encodermain start（C 版主控，固件 3.2.0 起）
#   encoder_main  -> (legacy) bash 版 S99zzencoder 已下线，仅保留占位不动作
#   listener/heartbeat -> 不单独拉起（skip，legacy 子进程随 bash 家族）
#   ipc_server (factoryinit 服务) -> /etc/init.d/S99factoryinit start
# 输出: {"restarted":"encodermain","skipped":"listener,heartbeat"}
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
        encodermain)
            # C 版主控：清理残留 pid 文件（S96encodermain 的 PIDFILE），
            # 避免 encalertd 显式 pidfile 探测在启动窗口期旧 pid 再次误判
            rm -f "$STATE_DIR/pid"
            /etc/init.d/S96encodermain start >/dev/null 2>&1
            echo "encodermain"
            ;;
        encoder_main)
            : # legacy bash 家族：S99zzencoder 已下线，不做动作
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
            majestic|encodermain|ipc_server) R="${R:+$R,}$line" ;;
            *) ;;
        esac
    done
    printf '{"restarted":"%s"}' "$R"
}
exit 0
