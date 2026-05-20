#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/state.sh"

LOG_TAIL_PID=""
CLEANUP_DONE="false"

start_live_log_stream() {
    if ! command_exists tail; then
        log_warn_tag "LIFECYCLE" "tail command missing, fallback to direct stdout only"
        return 1
    fi

    tail -n 0 -f "$LOGFILE" &
    LOG_TAIL_PID=$!
    return 0
}

wait_for_register_ready() {
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
    log_info_tag "LIFECYCLE" "starting heartbeat and listener after register success"
    sh "$APP_HOME/app_service.sh" heartbeat &
    sleep 1
    sh "$APP_HOME/app_service.sh" listener &
    sleep 1
}

cleanup_main() {
    [ "$CLEANUP_DONE" = "false" ] || return 0
    CLEANUP_DONE="true"

    export ENCODER_STDOUT_LOG=1
    log_info_tag "LIFECYCLE" "encoder main cleanup start"
    stop_pidfile_process "$LISTENER_PID_FILE"
    stop_pidfile_process "$HEARTBEAT_PID_FILE"
    stop_pidfile_process "$SEGMENT_WORKER_PID_FILE"
    release_pidfile_if_owner "$MAIN_PID_FILE"

    if [ -n "$LOG_TAIL_PID" ] && kill -0 "$LOG_TAIL_PID" 2>/dev/null; then
        kill "$LOG_TAIL_PID" 2>/dev/null
        wait "$LOG_TAIL_PID" 2>/dev/null
    fi

    log_info_tag "LIFECYCLE" "encoder main cleanup finished"
}

ensure_layout
state_init
print_title "$PROJECT_TITLE Main"

ensure_device_id_configured || exit 1

if ! claim_pidfile "$MAIN_PID_FILE"; then
    log_error "encoder main already running"
    exit 1
fi

trap 'cleanup_main; exit 0' INT TERM
trap 'release_pidfile_if_owner "$MAIN_PID_FILE"' EXIT

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
