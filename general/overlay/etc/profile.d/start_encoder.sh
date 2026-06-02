#!/bin/sh

# 登录完成后延迟启动编码器控制程序。
# 本脚本由 encoder.sh 在后台调用，不会占用串口或 SSH 登录终端。
# /etc/profile 自动读取本文件时不启动程序，只有 encoder.sh 传入 --run 时才执行。
[ "${1:-}" = "--run" ] || return 0 2>/dev/null || exit 0

# 等待板端基础服务稳定的秒数。现场需要更长等待时间时，直接修改这里。
START_DELAY_SEC="${START_DELAY_SEC:-10}"

# 以下路径均为板端绝对路径，不依赖调用脚本时所在的当前目录。
ENCODER_HOME="${ENCODER_HOME:-/root/encoder}"
ENCODER_MAIN_SCRIPT="${ENCODER_MAIN_SCRIPT:-${ENCODER_HOME}/encoder_main.sh}"
JQ_SOURCE_FILE="${JQ_SOURCE_FILE:-/mnt/mmcblk0p1/jq-linux-armhf}"
JQ_LINK_FILE="${JQ_LINK_FILE:-/usr/bin/jq}"

# 启动锁用于避免多次登录时重复创建等待任务。
START_STATE_DIR="${START_STATE_DIR:-${ENCODER_HOME}/runtime/state}"
START_LOCK_DIR="${START_LOCK_DIR:-${START_STATE_DIR}/start_encoder.lock}"
START_LOG_DIR="${START_LOG_DIR:-${ENCODER_HOME}/runtime/logs}"
START_LOG_FILE="${START_LOG_FILE:-${START_LOG_DIR}/encoder_autostart.log}"
MAIN_PID_FILE="${MAIN_PID_FILE:-${START_STATE_DIR}/encoder_main.pid}"

log_info() {
    echo "$(date '+%F %T') [start_encoder.sh] [INFO] $*"
}

log_error() {
    echo "$(date '+%F %T') [start_encoder.sh] [ERROR] $*" >&2
}

log_warn() {
    echo "$(date '+%F %T') [start_encoder.sh] [WARN] $*" >&2
}

fail() {
    log_error "$*"
    exit 1
}

is_pid_running_file() {
    pid_file="$1"
    pid=$(cat "$pid_file" 2>/dev/null)
    case "$pid" in
        ''|*[!0-9]*)
            return 1
            ;;
    esac

    kill -0 "$pid" 2>/dev/null
}

release_start_lock() {
    lock_owner=$(cat "$START_LOCK_DIR/pid" 2>/dev/null)
    [ "$lock_owner" = "$$" ] || return 0
    rm -rf "$START_LOCK_DIR"
}

claim_start_lock() {
    mkdir -p "$START_STATE_DIR" "$START_LOG_DIR" || return 1

    if mkdir "$START_LOCK_DIR" 2>/dev/null; then
        echo "$$" > "$START_LOCK_DIR/pid"
        return 0
    fi

    lock_owner=$(cat "$START_LOCK_DIR/pid" 2>/dev/null)
    if [ -n "$lock_owner" ] && kill -0 "$lock_owner" 2>/dev/null; then
        log_info "startup task already waiting pid=$lock_owner"
        return 2
    fi

    # 上一次异常退出可能遗留锁目录，确认没有存活进程后再清理。
    rm -rf "$START_LOCK_DIR"
    mkdir "$START_LOCK_DIR" 2>/dev/null || return 1
    echo "$$" > "$START_LOCK_DIR/pid"
}

case "$START_DELAY_SEC" in
    ''|*[!0-9]*)
        fail "START_DELAY_SEC must be a non-negative integer: $START_DELAY_SEC"
        ;;
esac

# DEVICE_ID 缺失时打印告警，但启动脚本继续执行。
# encoder_main.sh 会按自身规则决定是否允许主程序继续运行。
if [ -z "${DEVICE_ID:-}" ]; then
    log_warn "DEVICE_ID is missing: encoder startup will continue"
fi

if is_pid_running_file "$MAIN_PID_FILE"; then
    log_info "encoder main already running pid=$(cat "$MAIN_PID_FILE")"
    exit 0
fi

claim_start_lock
lock_rc=$?
case "$lock_rc" in
    0)
        ;;
    2)
        exit 0
        ;;
    *)
        fail "cannot claim startup lock: $START_LOCK_DIR"
        ;;
esac

trap 'release_start_lock' EXIT
trap 'exit 0' INT TERM

log_info "wait ${START_DELAY_SEC}s for board services to become stable"
sleep "$START_DELAY_SEC"

# 等待期间可能由其它登录会话启动成功，因此启动前再次检查。
if is_pid_running_file "$MAIN_PID_FILE"; then
    log_info "encoder main already running pid=$(cat "$MAIN_PID_FILE")"
    exit 0
fi

# jq 源文件必须存在。缺失时直接报错，不启动 encoder_main.sh。
[ -f "$JQ_SOURCE_FILE" ] || fail "jq source file not found: /mnt/mmcblk0p1/jq-linux-armhf"

# 只补充执行权限，不写死为 755 或 777。
chmod +x "$JQ_SOURCE_FILE" || fail "cannot add execute permission: /mnt/mmcblk0p1/jq-linux-armhf"

# 如果 /usr/bin/jq 已经是普通文件，拒绝覆盖，避免破坏板子原有程序。
if [ -e "$JQ_LINK_FILE" ] && [ ! -L "$JQ_LINK_FILE" ]; then
    fail "jq link target exists and is not a symbolic link: /usr/bin/jq"
fi

ln -sfn "$JQ_SOURCE_FILE" "$JQ_LINK_FILE" || fail "cannot create jq symbolic link: /usr/bin/jq"
"$JQ_LINK_FILE" --version >/dev/null 2>&1 || fail "jq executable check failed: /usr/bin/jq"

[ -f "$ENCODER_MAIN_SCRIPT" ] || fail "encoder main script not found: /root/encoder/encoder_main.sh"
command -v nohup >/dev/null 2>&1 || fail "required command missing: nohup"

# 后台启动主程序，避免占用串口或 SSH 登录后的交互终端。
nohup sh "$ENCODER_MAIN_SCRIPT" >> "$START_LOG_FILE" 2>&1 &
encoder_pid=$!
sleep 1

if ! kill -0 "$encoder_pid" 2>/dev/null; then
    fail "encoder main exited immediately, check log: $START_LOG_FILE"
fi

log_info "encoder main start requested pid=$encoder_pid log=$START_LOG_FILE"
