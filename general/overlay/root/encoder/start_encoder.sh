#!/bin/sh

# 开机服务完成后，由 /etc/init.d/S99zzencoder 调用本脚本。
# S99zzencoder 会将本脚本放到后台，因此等待过程不会阻塞系统启动。

# 等待板端基础服务稳定的秒数。现场需要更长等待时间时，直接修改这里。
START_DELAY_SEC="${START_DELAY_SEC:-10}"

# 编码器程序目录和入口脚本。
ENCODER_HOME="${ENCODER_HOME:-/root/encoder}"
ENCODER_MAIN_SCRIPT="${ENCODER_MAIN_SCRIPT:-${ENCODER_HOME}/encoder_main.sh}"

# jq 工具存放在 TF 卡中，启动前为它补充执行权限并建立系统软连接。
JQ_SOURCE_FILE="${JQ_SOURCE_FILE:-/mnt/mmcblk0p1/jq-linux-armhf}"
JQ_LINK_FILE="${JQ_LINK_FILE:-/usr/bin/jq}"

# 启动锁用于避免重复创建等待任务。
START_STATE_DIR="${START_STATE_DIR:-${ENCODER_HOME}/runtime/state}"
START_LOCK_DIR="${START_LOCK_DIR:-${START_STATE_DIR}/start_encoder.lock}"
START_LOG_DIR="${START_LOG_DIR:-${ENCODER_HOME}/runtime/logs}"
START_LOG_FILE="${START_LOG_FILE:-${START_LOG_DIR}/encoder_autostart.log}"
MAIN_PID_FILE="${MAIN_PID_FILE:-${START_STATE_DIR}/encoder_main.pid}"
FW_PRINTENV_CMD="${FW_PRINTENV_CMD:-fw_printenv}"

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
        printf '%s\n' "$$" > "$START_LOCK_DIR/pid"
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
    printf '%s\n' "$$" > "$START_LOCK_DIR/pid"
}

case "$START_DELAY_SEC" in
    ''|*[!0-9]*)
        fail "START_DELAY_SEC must be a non-negative integer: $START_DELAY_SEC"
        ;;
esac

# 自动启动统一从 U-Boot 环境读取设备 ID，再导出给 encoder_main.sh。
load_device_id_from_uboot

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

# 等待期间可能由其它流程启动成功，因此启动前再次检查。
if is_pid_running_file "$MAIN_PID_FILE"; then
    log_info "encoder main already running pid=$(cat "$MAIN_PID_FILE")"
    exit 0
fi

# jq 源文件必须存在。缺失时直接报错，不启动 encoder_main.sh。
[ -f "$JQ_SOURCE_FILE" ] || fail "jq source file not found: $JQ_SOURCE_FILE"
log_info "jq source file detected: $JQ_SOURCE_FILE"

# 只补充执行权限，不写死为 755 或 777。
chmod +x "$JQ_SOURCE_FILE" || fail "cannot add execute permission: $JQ_SOURCE_FILE"
log_info "jq execute permission ready: $JQ_SOURCE_FILE"

# 如果 /usr/bin/jq 已经是普通文件，拒绝覆盖，避免破坏板子原有程序。
if [ -e "$JQ_LINK_FILE" ] && [ ! -L "$JQ_LINK_FILE" ]; then
    fail "jq link target exists and is not a symbolic link: $JQ_LINK_FILE"
fi

ln -sfn "$JQ_SOURCE_FILE" "$JQ_LINK_FILE" || fail "cannot create jq symbolic link: $JQ_LINK_FILE"
log_info "jq symbolic link ready: $JQ_LINK_FILE -> $JQ_SOURCE_FILE"

jq_version=$("$JQ_LINK_FILE" --version 2>/dev/null) || fail "jq executable check failed: $JQ_LINK_FILE"
log_info "jq executable check success: path=$JQ_LINK_FILE version=$jq_version"

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
