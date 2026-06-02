#!/bin/sh

# 编码器主入口脚本。
# 负责初始化运行目录/状态、完成注册、启动心跳与 MQTT 监听，并在后台服务异常退出时拉起。
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/state.sh"
. "$SCRIPT_DIR/led.sh"

# 日志镜像 tail 进程的 PID。主进程退出时需要一起清理，避免残留后台进程。
LOG_TAIL_PID=""
CLEANUP_DONE="false"

start_live_log_stream() {
    # 运行在板子终端时，把日志文件实时镜像到当前 stdout，便于现场调试。
    if ! command_exists tail; then
        log_warn_tag "LIFECYCLE" "tail command missing, fallback to direct stdout only"
        return 1
    fi

    tail -n 0 -f "$LOGFILE" &
    LOG_TAIL_PID=$!
    return 0
}

wait_for_register_ready() {
    # 注册依赖云端下发 FTP/SRS 等运行时配置，失败时持续重试。
    while true; do
        if sh "$APP_HOME/app_service.sh" register; then
            log_info_tag "LIFECYCLE" "register finished with success"
            return 0
        fi

        log_warn_tag "LIFECYCLE" "register failed, retry after ${REGISTER_RETRY_INTERVAL_SEC}s"
        sleep "$REGISTER_RETRY_INTERVAL_SEC"
    done
}

start_runtime_services() {
    # 注册成功后才启动心跳和监听，保证后续命令使用的是最新运行时配置。
    log_info_tag "LIFECYCLE" "starting heartbeat and listener after register success"
    sh "$APP_HOME/app_service.sh" heartbeat &
    sleep 1
    sh "$APP_HOME/app_service.sh" listener &
    sleep 1
}

cleanup_main() {
    # 信号 trap 可能被多次触发，用 CLEANUP_DONE 保证清理逻辑只执行一次。
    [ "$CLEANUP_DONE" = "false" ] || return 0
    CLEANUP_DONE="true"

    export ENCODER_STDOUT_LOG=1
    log_info_tag "LIFECYCLE" "encoder main cleanup start"
    stop_pidfile_process "$LISTENER_PID_FILE"
    stop_pidfile_process "$HEARTBEAT_PID_FILE"
    stop_pidfile_process "$SEGMENT_WORKER_PID_FILE"
    stop_pidfile_process "$VOICE_PLAYER_PID_FILE"
    release_pidfile_if_owner "$MAIN_PID_FILE"
    led_runtime_stop

    if [ -n "$LOG_TAIL_PID" ] && kill -0 "$LOG_TAIL_PID" 2>/dev/null; then
        kill "$LOG_TAIL_PID" 2>/dev/null
        wait "$LOG_TAIL_PID" 2>/dev/null
    fi

    log_info_tag "LIFECYCLE" "encoder main cleanup finished"
}

# 主启动流程：目录/状态初始化 -> device_id 校验 -> 主进程锁 -> 注册 -> 启动服务。
ensure_layout
state_init
print_title "$PROJECT_TITLE Main"

ensure_device_id_configured || exit 1

if ! claim_pidfile "$MAIN_PID_FILE"; then
    log_error "encoder main already running"
    exit 1
fi

trap 'exit 0' INT TERM
trap 'cleanup_main' EXIT

# While the controller is running, an all-off board means idle and ready for commands.
led_runtime_start

log_info_tag "LIFECYCLE" "encoder main start device_id=$DEVICE_ID version=$DEVICE_VERSION"
log_info_tag "LIFECYCLE" "mqtt endpoint=${MQTT_HOST}:${MQTT_PORT} subscribe_topic=$MQTT_SUBSCRIBE_TOPIC"
log_info_tag "LIFECYCLE" "logfile=$LOGFILE initial_idle=$(state_get_idle) recording=$(state_get_recording) publishing=$(state_get_publishing)"

if start_live_log_stream; then
    export ENCODER_STDOUT_LOG=0
    log_info_tag "LIFECYCLE" "live log mirror started pid=$LOG_TAIL_PID"
else
    log_warn_tag "LIFECYCLE" "live log mirror not started; using direct stdout only"
fi

wait_for_register_ready
start_runtime_services

log_info_tag "LIFECYCLE" "encoder main enters supervision loop"

while true; do
    # 两个常驻子服务都有 pidfile，主进程只做轻量守护和自动重启。
    if ! stream_service_is_running; then
        log_warn_tag "LIFECYCLE" "Majestic service lost, restarting"
        stream_service_start_if_missing || log_error_tag "LIFECYCLE" "Majestic service restart failed"
    fi

    if ! is_pid_running_file "$HEARTBEAT_PID_FILE"; then
        log_warn_tag "LIFECYCLE" "heartbeat service lost, restarting"
        sh "$APP_HOME/app_service.sh" heartbeat &
        sleep 1
    fi

    if ! is_pid_running_file "$LISTENER_PID_FILE"; then
        log_warn_tag "LIFECYCLE" "listener service lost, restarting"
        sh "$APP_HOME/app_service.sh" listener &
        sleep 1
    fi

    sleep 2
done
