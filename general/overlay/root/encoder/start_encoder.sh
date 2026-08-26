#!/bin/sh

# 开机服务完成后，由 /etc/init.d/S99zzencoder 调用本脚本。
# S99zzencoder 会将本脚本放到后台，因此等待过程不会阻塞系统启动。

# 等待板端基础服务稳定的秒数。现场需要更长等待时间时，直接修改这里。
START_DELAY_SEC="${START_DELAY_SEC:-10}"

# 编码器程序目录和入口脚本。
ENCODER_HOME="${ENCODER_HOME:-/root/encoder}"
ENCODER_MAIN_SCRIPT="${ENCODER_MAIN_SCRIPT:-${ENCODER_HOME}/encoder_main.sh}"

# 固件内置资源目录。jq 放在这里，启动时把该目录放到 PATH 前面供协议脚本调用。
RESOURCE_DIR="${RESOURCE_DIR:-/root/resources}"
JQ_BINARY_FILE="${JQ_BINARY_FILE:-${RESOURCE_DIR}/jq}"
START_ENCODER_SCRIPT="${START_ENCODER_SCRIPT:-${ENCODER_HOME}/start_encoder.sh}"

# 启动锁用于避免重复创建等待任务。
START_STATE_DIR="${START_STATE_DIR:-${ENCODER_HOME}/runtime/state}"
START_LOCK_DIR="${START_LOCK_DIR:-${START_STATE_DIR}/start_encoder.lock}"
START_LOG_DIR="${START_LOG_DIR:-${ENCODER_HOME}/runtime/logs}"
START_LOG_FILE="${START_LOG_FILE:-${START_LOG_DIR}/encoder_autostart.log}"
MAIN_PID_FILE="${MAIN_PID_FILE:-${START_STATE_DIR}/encoder_main.pid}"
FW_PRINTENV_CMD="${FW_PRINTENV_CMD:-fw_printenv}"

ENCODER_CONFIG_FILE="${ENCODER_CONFIG_FILE:-${ENCODER_HOME}/config.sh}"
[ -f "$ENCODER_CONFIG_FILE" ] && . "$ENCODER_CONFIG_FILE"

LOCAL_TIMEZONE="${LOCAL_TIMEZONE:-CST-8}"
TZ="$LOCAL_TIMEZONE"
export TZ

LISTENER_PID_FILE="${LISTENER_PID_FILE:-${START_STATE_DIR}/listener.pid}"
HEARTBEAT_PID_FILE="${HEARTBEAT_PID_FILE:-${START_STATE_DIR}/heartbeat.pid}"
SEGMENT_WORKER_PID_FILE="${SEGMENT_WORKER_PID_FILE:-${START_STATE_DIR}/segment_worker.pid}"
VOICE_PLAYER_PID_FILE="${VOICE_PLAYER_PID_FILE:-${START_STATE_DIR}/voice_player.pid}"
LED_UPLOAD_BLINK_PID_FILE="${LED_UPLOAD_BLINK_PID_FILE:-${START_STATE_DIR}/led_upload_blink.pid}"
MAIN_STOPPED_RESTORE_LOCK_DIR="${MAIN_STOPPED_RESTORE_LOCK_DIR:-${START_STATE_DIR}/main_stopped_restore.lock}"
VOICE_PLAYER_LOCK_DIR="${VOICE_PLAYER_LOCK_DIR:-${VOICE_PLAYER_PID_FILE}.lock}"
CHILD_EXIT_CHECK_SEC="${CHILD_EXIT_CHECK_SEC:-1}"
CHILD_EXIT_WAIT_SEC="${CHILD_EXIT_WAIT_SEC:-15}"

log_info() {
    printf '%s [start_encoder.sh] [INFO] %s\n' "$(date '+%F %T')" "$*"
}

log_error() {
    printf '%s [start_encoder.sh] [ERROR] %s\n' "$(date '+%F %T')" "$*" >&2
}

log_warn() {
    printf '%s [start_encoder.sh] [WARN] %s\n' "$(date '+%F %T')" "$*" >&2
}

fail() {
    log_error "$*"
    exit 1
}

load_device_id_from_uboot() {
    # DEVICE_ID 持久化在 U-Boot 环境中；启动主程序前将它转换成 Linux 进程环境变量。
    command -v "$FW_PRINTENV_CMD" >/dev/null 2>&1 || {
        fail "required command missing: $FW_PRINTENV_CMD"
    }

    uboot_device_id=$("$FW_PRINTENV_CMD" -n DEVICE_ID 2>/dev/null | sed -n '1p' | tr -d '\r\n')
    [ -n "$uboot_device_id" ] || {
        fail "DEVICE_ID not found in U-Boot environment: $FW_PRINTENV_CMD -n DEVICE_ID"
    }

    case "$uboot_device_id" in
        *[!A-Za-z0-9._-]*)
            fail "invalid DEVICE_ID from U-Boot environment: $uboot_device_id"
            ;;
    esac

    DEVICE_ID="$uboot_device_id"
    DEVICE_ID_ORIGIN="uboot-env:DEVICE_ID"
    export DEVICE_ID DEVICE_ID_ORIGIN
    log_info "DEVICE_ID loaded from U-Boot environment: $DEVICE_ID"
}

pid_matches_command() {
    pid="$1"
    expected_command="$2"

    case "$pid" in
        ''|*[!0-9]*)
            return 1
            ;;
    esac

    kill -0 "$pid" 2>/dev/null || return 1
    [ -n "$expected_command" ] || return 0
    [ -r "/proc/$pid/cmdline" ] || return 1

    tr '\000' ' ' < "/proc/$pid/cmdline" | grep -F "$expected_command" >/dev/null 2>&1
}

is_pid_running_file() {
    pid_file="$1"
    expected_command="$2"
    pid=$(cat "$pid_file" 2>/dev/null)

    pid_matches_command "$pid" "$expected_command"
}

encoder_main_process_is_running() {
    ps w 2>/dev/null | awk -v self="$$" '
        NR > 1 && $1 != self && $0 ~ /encoder_main\.sh/ {
            found = 1
        }
        END { exit(found ? 0 : 1) }
    '
}

encoder_main_is_running() {
    is_pid_running_file "$MAIN_PID_FILE" "encoder_main.sh" && return 0
    encoder_main_process_is_running
}

append_active_service() {
    item="$1"
    if [ -n "$active_services" ]; then
        active_services="${active_services},${item}"
    else
        active_services="$item"
    fi
}

check_child_pidfile() {
    pid_file="$1"
    expected_command="$2"
    label="$3"
    pid=$(cat "$pid_file" 2>/dev/null)

    if pid_matches_command "$pid" "$expected_command"; then
        append_active_service "${label}:${pid}"
        return 0
    fi

    if [ -f "$pid_file" ]; then
        log_warn "remove stale child pidfile label=$label pid=${pid:-empty} file=$pid_file"
        rm -f "$pid_file"
    fi

    return 1
}

check_child_lock_dir() {
    lock_dir="$1"
    label="$2"

    [ -d "$lock_dir" ] || return 1
    lock_owner=$(cat "$lock_dir/pid" 2>/dev/null)
    if [ -n "$lock_owner" ] && kill -0 "$lock_owner" 2>/dev/null; then
        append_active_service "${label}:${lock_owner}"
        return 0
    fi

    log_warn "remove stale child lock label=$label owner=${lock_owner:-empty} dir=$lock_dir"
    rm -rf "$lock_dir"
    return 1
}

scan_encoder_child_processes() {
    self_pid="$$"
    ps w 2>/dev/null |
        grep "$ENCODER_HOME" |
        grep -E 'app_service\.sh|voice\.sh|led\.sh' |
        grep -v grep |
        while read -r pid rest; do
            [ -n "$pid" ] || continue
            [ "$pid" = "$self_pid" ] && continue
            printf '%s:%s ' "$pid" "$rest"
        done
}

collect_active_child_services() {
    active_services=""

    check_child_pidfile "$HEARTBEAT_PID_FILE" "app_service.sh heartbeat" "heartbeat" || true
    check_child_pidfile "$LISTENER_PID_FILE" "app_service.sh listener" "listener" || true
    check_child_pidfile "$SEGMENT_WORKER_PID_FILE" "app_service.sh segment_worker" "segment_worker" || true
    check_child_pidfile "$VOICE_PLAYER_PID_FILE" "voice.sh" "voice" || true
    check_child_pidfile "$LED_UPLOAD_BLINK_PID_FILE" "led.sh upload_blink_worker" "led_upload_blink" || true
    check_child_lock_dir "$MAIN_STOPPED_RESTORE_LOCK_DIR" "majestic_restore" || true
    check_child_lock_dir "$VOICE_PLAYER_LOCK_DIR" "voice_lock" || true

    scanned_children=$(scan_encoder_child_processes)
    [ -n "$scanned_children" ] && append_active_service "process:${scanned_children}"
}

wait_for_previous_child_services_to_exit() {
    waited_sec=0

    while true; do
        collect_active_child_services
        if [ -z "$active_services" ]; then
            log_info "old child services are fully stopped"
            return 0
        fi

        if [ "$waited_sec" -ge "$CHILD_EXIT_WAIT_SEC" ]; then
            fail "old child services still active after ${CHILD_EXIT_WAIT_SEC}s: $active_services"
        fi

        log_warn "wait old child services to exit before restart: $active_services"
        sleep "$CHILD_EXIT_CHECK_SEC"
        waited_sec=$((waited_sec + CHILD_EXIT_CHECK_SEC))
    done
}

release_start_lock() {
    lock_owner=$(cat "$START_LOCK_DIR/pid" 2>/dev/null)
    [ "$lock_owner" = "$$" ] || return 0
    rm -rf "$START_LOCK_DIR"
}

claim_start_lock() {
    mkdir -p "$START_STATE_DIR" "$START_LOG_DIR" || return 1

    if mkdir "$START_LOCK_DIR" 2>/dev/null; then
        printf '%s\n' "$$" > "$START_LOCK_DIR/pid"
        return 0
    fi

    lock_owner=$(cat "$START_LOCK_DIR/pid" 2>/dev/null)
    if pid_matches_command "$lock_owner" "start_encoder.sh"; then
        log_info "startup task already waiting pid=$lock_owner"
        return 2
    fi

    # 上一次异常退出可能遗留锁目录，确认没有存活进程后再清理。
    rm -rf "$START_LOCK_DIR"
    mkdir "$START_LOCK_DIR" 2>/dev/null || return 1
    printf '%s\n' "$$" > "$START_LOCK_DIR/pid"
}

prepare_jq() {
    [ -f "$JQ_BINARY_FILE" ] || fail "jq file not found: $JQ_BINARY_FILE"
    [ -x "$JQ_BINARY_FILE" ] || chmod +x "$JQ_BINARY_FILE" ||
        fail "cannot add execute permission: $JQ_BINARY_FILE"

    jq_dir=$(CDPATH= cd "$(dirname "$JQ_BINARY_FILE")" && pwd) ||
        fail "cannot resolve jq directory: $JQ_BINARY_FILE"

    case ":$PATH:" in
        *:"$jq_dir":*)
            ;;
        *)
            PATH="$jq_dir:$PATH"
            export PATH
            ;;
    esac

    jq_command=$(command -v jq 2>/dev/null) ||
        fail "jq command not found after PATH update: $jq_dir"
    [ "$jq_command" = "$jq_dir/jq" ] ||
        fail "jq command path mismatch: command=$jq_command expected=$jq_dir/jq"

    jq_version=$(jq --version 2>/dev/null) ||
        fail "jq executable check failed: $JQ_BINARY_FILE"
    log_info "jq executable ready: path=$JQ_BINARY_FILE version=$jq_version"
}

case "$START_DELAY_SEC" in
    ''|*[!0-9]*)
        fail "START_DELAY_SEC must be a non-negative integer: $START_DELAY_SEC"
        ;;
esac

case "$CHILD_EXIT_CHECK_SEC" in
    ''|*[!0-9]*|0)
        fail "CHILD_EXIT_CHECK_SEC must be a positive integer: $CHILD_EXIT_CHECK_SEC"
        ;;
esac

case "$CHILD_EXIT_WAIT_SEC" in
    ''|*[!0-9]*)
        fail "CHILD_EXIT_WAIT_SEC must be a non-negative integer: $CHILD_EXIT_WAIT_SEC"
        ;;
esac

# 自动启动统一从 U-Boot 环境读取设备 ID，再导出给 encoder_main.sh。
load_device_id_from_uboot

if encoder_main_is_running; then
    main_pid=$(cat "$MAIN_PID_FILE" 2>/dev/null)
    log_info "encoder main already running pid=${main_pid:-unknown}"
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

# 等待期间可能由其它流程启动成功，因此启动前再次检查。
if encoder_main_is_running; then
    main_pid=$(cat "$MAIN_PID_FILE" 2>/dev/null)
    log_info "encoder main already running pid=${main_pid:-unknown}"
    exit 0
fi

wait_for_previous_child_services_to_exit
prepare_jq

[ -f "$ENCODER_MAIN_SCRIPT" ] || fail "encoder main script not found: $ENCODER_MAIN_SCRIPT"
command -v nohup >/dev/null 2>&1 || fail "required command missing: nohup"

# 后台启动主程序，避免占用开机服务进程。
nohup sh "$ENCODER_MAIN_SCRIPT" >> "$START_LOG_FILE" 2>&1 &
encoder_pid=$!
sleep 1

if ! kill -0 "$encoder_pid" 2>/dev/null; then
    fail "encoder main exited immediately, check log: $START_LOG_FILE"
fi

log_info "encoder main start requested pid=$encoder_pid log=$START_LOG_FILE"
