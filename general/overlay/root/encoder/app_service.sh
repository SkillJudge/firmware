#!/bin/sh

# 服务层入口。
# `register`、`heartbeat`、`listener`、`dispatch`、`segment_worker` 都从这里进入，方便主脚本用同一个文件启动不同角色。
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/feature_engine.sh"
. "$SCRIPT_DIR/battery.sh"

service_log_mqtt_receive() {
    # 原始 MQTT 包可能很长，默认只打印 topic；需要排查协议时再打开 MQTT_PAYLOAD_VERBOSE。
    topic="$1"
    payload="$2"

    case "$topic" in
        */heartbeat/heartbeat_ack)
            [ "$HEARTBEAT_LOG_VERBOSE" = "true" ] || return 0
            ;;
    esac

    if [ "$MQTT_PAYLOAD_VERBOSE" = "true" ]; then
        log_info_tag "MQTT-RECV" "topic=$topic payload=$payload"
    else
        log_info_tag "MQTT-RECV" "topic=$topic"
    fi
}

service_register_ack_listener() {
    # 注册时临时订阅 register_ack，只接收和本次 msgId 匹配的应答。
    topic="$1"
    expected_msg_id="$2"
    response_file="$3"

    mqtt_sub_once "$topic" "$REGISTER_ACK_TIMEOUT_SEC" 1 2>/dev/null | while IFS= read -r ack_payload; do
        [ -n "$ack_payload" ] || continue

        service_log_mqtt_receive "$topic" "$ack_payload"

        ack_reply_to=$(json_get_number "$ack_payload" "data.replyTo")
        ack_msg_id=$(json_get_number "$ack_payload" "msgId")

        if [ "$ack_reply_to" = "$expected_msg_id" ] || [ "$ack_msg_id" = "$expected_msg_id" ]; then
            printf '%s\n' "$ack_payload" > "$response_file"
            break
        fi
    done
}

service_apply_register_runtime() {
    # 云端 registerAck 会下发 FTP/SRS 参数和服务器时间，这些值写入 runtime 状态文件。
    response_json="$1"

    ftp_host=$(json_get_string "$response_json" "data.ftp.host")
    ftp_port=$(json_get_string "$response_json" "data.ftp.port")
    ftp_user=$(json_get_string "$response_json" "data.ftp.username")
    ftp_pass=$(json_get_string "$response_json" "data.ftp.password")
    srs_host=$(json_get_string "$response_json" "data.srs.host")
    srs_port=$(json_get_string "$response_json" "data.srs.port")
    srs_user=$(json_get_string "$response_json" "data.srs.username")
    srs_pass=$(json_get_string "$response_json" "data.srs.password")
    server_timestamp=$(json_get_number "$response_json" "data.timestamp")

    save_runtime_ftp_config "$ftp_host" "$ftp_port" "$ftp_user" "$ftp_pass"
    save_runtime_srs_config "$srs_host" "$srs_port" "$srs_user" "$srs_pass"

    if [ -n "$server_timestamp" ]; then
        sync_time_from_timestamp_ms "$server_timestamp"
    else
        log_warn "register ack has no timestamp, skip time sync"
    fi

    log_info_tag "REGISTER" "runtime applied ftp_host=${ftp_host:-$FTP_HOST} ftp_port=${ftp_port:-$FTP_PORT} srs_host=${srs_host:-$SRS_HOST} srs_port=${srs_port:-$SRS_PORT}"
}

service_register() {
    # 向控制服务注册设备。注册成功后，后续上传/推流优先使用 registerAck 下发的运行时配置。
    ensure_layout
    state_init
    ensure_runtime_files
    ensure_mqtt_tools || return 1

    msg_id=$(next_msgid)
    payload=$(protocol_build_register_payload "$msg_id") || return 1
    response_file="${TMP_DIR}/register_ack_${msg_id}.json"
    : > "$response_file"

    (
        service_register_ack_listener "$MQTT_REGISTER_ACK_TOPIC" "$msg_id" "$response_file"
    ) &
    ack_listener_pid=$!

    if [ "$MQTT_PAYLOAD_VERBOSE" = "true" ]; then
        log_info_tag "MQTT-PUB" "topic=$MQTT_REGISTER_TOPIC payload=$payload"
    else
        log_info_tag "MQTT-PUB" "topic=$MQTT_REGISTER_TOPIC msg=register"
    fi
    if mqtt_pub_json "$MQTT_REGISTER_TOPIC" "$payload"; then
        wait "$ack_listener_pid" 2>/dev/null
        response_json=$(cat "$response_file" 2>/dev/null)
        rm -f "$response_file"

        if [ -n "$response_json" ]; then
            ack_code=$(json_get_number "$response_json" "data.code")
            ack_desc=$(json_get_string "$response_json" "data.desc")
            log_info_tag "REGISTER" "ack received code=${ack_code:-unknown} desc=${ack_desc:-empty}"
            if [ "${ack_code:-1}" = "0" ]; then
                service_apply_register_runtime "$response_json"
                return 0
            fi
        fi

        log_warn_tag "REGISTER" "ack missing or non-zero"
        return 1
    fi

    kill "$ack_listener_pid" 2>/dev/null
    rm -f "$response_file"
    log_error_tag "REGISTER" "publish failed"
    return 1
}

service_heartbeat_once() {
    # 发布一次心跳。心跳内容来自 state 文件，反映当前空闲、录像、推流、电量等状态。
    ensure_layout
    state_init
    ensure_runtime_files
    ensure_mqtt_tools || return 1

    battery_refresh_state

    msg_id=$(next_msgid)
    payload=$(protocol_build_heartbeat_payload "$msg_id") || return 1
    if [ "$HEARTBEAT_LOG_VERBOSE" = "true" ]; then
        if [ "$MQTT_PAYLOAD_VERBOSE" = "true" ]; then
            log_info_tag "MQTT-PUB" "topic=$MQTT_HEARTBEAT_TOPIC payload=$payload"
        else
            log_info_tag "MQTT-PUB" "topic=$MQTT_HEARTBEAT_TOPIC msg=heartbeat"
        fi
    fi
    mqtt_pub_json "$MQTT_HEARTBEAT_TOPIC" "$payload"
}

service_heartbeat_forever() {
    # 常驻心跳服务。通过 pidfile 保证同一设备目录下只跑一个心跳进程。
    ensure_layout
    state_init
    ensure_runtime_files

    if ! claim_pidfile "$HEARTBEAT_PID_FILE"; then
        log_warn "heartbeat service already running"
        exit 0
    fi

    trap 'release_pidfile_if_owner "$HEARTBEAT_PID_FILE"' EXIT INT TERM
    log_info_tag "LIFECYCLE" "heartbeat service start interval=${HEARTBEAT_INTERVAL_SEC}s"

    while true; do
        service_heartbeat_once
        sleep "$HEARTBEAT_INTERVAL_SEC"
    done
}

service_dispatch() {
    # 单条 MQTT 命令的解析和分发入口。真正的业务动作在 feature_engine.sh 内完成。
    topic="$1"
    payload="$2"

    ensure_layout
    state_init
    ensure_runtime_files
    ensure_mqtt_tools || return 1
    feature_result_reset

    if ! protocol_parse_command "$topic" "$payload"; then
        log_error "dispatch parse failed topic=$topic error=$PROTO_ERROR payload=$payload"
        exit 1
    fi

    case "$PROTO_COMMAND" in
        ignore)
            log_debug_tag "DISPATCH" "ignore topic=$topic"
            exit 0
            ;;
        *)
            log_info_tag "DISPATCH" "command=$PROTO_COMMAND msg_id=$PROTO_MSG_ID task_id=$PROTO_TASK_ID record_id=$PROTO_RECORD_ID stream_url=$PROTO_STREAM_URL"
            ;;
    esac

    case "$PROTO_COMMAND" in
        # 普通录像、推流、抓拍命令。
        record_start)
            feature_record_start record "$PROTO_RECORD_ID" "$PROTO_TASK_ID"
            rc=$?
            ;;
        record_stop)
            feature_record_stop record "$PROTO_RECORD_ID"
            rc=$?
            ;;
        capture_take)
            feature_capture_take "$PROTO_CAPTURE_ID"
            rc=$?
            ;;
        stream_start)
            feature_stream_start stream "$PROTO_STREAM_URL" "$PROTO_DURATION" "$PROTO_TASK_ID"
            rc=$?
            ;;
        stream_stop)
            feature_stream_stop stream "$PROTO_REASON"
            rc=$?
            ;;
        audio_play)
            feature_audio_play "$PROTO_FILE_NAME" "$PROTO_FILE_PATH"
            rc=$?
            ;;
        task_prepare_voice)
            feature_task_prepare_voice "$PROTO_TASK_ID"
            rc=$?
            ;;
        task_prepare_desk_voice)
            feature_task_prepare_desk_voice "$PROTO_TASK_ID"
            rc=$?
            ;;
        # 实验任务使用 task/* 协议，回包格式和普通 record/stream 略有区别。
        task_stream_start)
            feature_stream_start task "$PROTO_STREAM_URL" "$PROTO_DURATION" "$PROTO_TASK_ID"
            rc=$?
            ;;
        task_record_start)
            feature_record_start task "$PROTO_RECORD_ID" "$PROTO_TASK_ID"
            rc=$?
            ;;
        task_hand_voice_start)
            feature_task_hand_voice_start "$PROTO_TASK_ID" "$PROTO_RECORD_ID"
            rc=$?
            ;;
        task_hand_voice_stop)
            feature_task_hand_voice_stop "$PROTO_TASK_ID" "$PROTO_RECORD_ID"
            rc=$?
            ;;
        task_record_stop)
            feature_record_stop task "$PROTO_RECORD_ID"
            rc=$?
            ;;
        task_reset)
            feature_reset_execute "$PROTO_TASK_ID"
            rc=$?
            ;;
        *)
            log_error "dispatch unsupported protocol command=$PROTO_COMMAND"
            exit 1
            ;;
    esac

    protocol_publish_command_result || rc=1
    exit "$rc"
}

service_listener_forever() {
    # 常驻 MQTT 监听服务。订阅进程通过 FIFO 交给当前 shell 读取，退出时可准确清理。
    ensure_layout
    state_init
    ensure_runtime_files
    ensure_mqtt_tools || exit 1
    require_command mkfifo || exit 1

    if ! claim_pidfile "$LISTENER_PID_FILE" "$APP_HOME/app_service.sh listener"; then
        log_warn "listener service already running"
        exit 0
    fi

    listener_fifo="${TMP_DIR}/listener_$$.fifo"
    listener_sub_pid=""

    service_listener_cleanup() {
        if [ -n "$listener_sub_pid" ] && kill -0 "$listener_sub_pid" 2>/dev/null; then
            kill "$listener_sub_pid" 2>/dev/null
            wait "$listener_sub_pid" 2>/dev/null
        fi
        rm -f "$listener_fifo"
        release_pidfile_if_owner "$LISTENER_PID_FILE"
    }

    rm -f "$listener_fifo"
    mkfifo "$listener_fifo" || {
        release_pidfile_if_owner "$LISTENER_PID_FILE"
        exit 1
    }

    trap 'service_listener_cleanup' EXIT INT TERM
    log_info_tag "LIFECYCLE" "listener subscribe topic=$MQTT_SUBSCRIBE_TOPIC"

    mqtt_sub_forever_with_topic "$MQTT_SUBSCRIBE_TOPIC" > "$listener_fifo" &
    listener_sub_pid=$!

    while IFS= read -r line; do
        [ -n "$line" ] || continue
        topic=${line%% *}
        payload=${line#* }
        [ "$topic" != "$line" ] || payload=""
        service_log_mqtt_receive "$topic" "$payload"
        sh "$APP_HOME/app_service.sh" dispatch "$topic" "$payload"
    done < "$listener_fifo"
}

service_segment_worker() {
    # 录像分片上传 worker，由 feature_record_start 拉起，停止录像或 reset 时会被清理。
    feature_record_segment_loop "$1" "$2" "$3"
}

# 命令行分派表。主程序和子进程都通过 `sh app_service.sh <role>` 调用这里。
case "$1" in
    register)
        ensure_device_id_configured || exit 1
        shift
        service_register "$@"
        ;;
    heartbeat)
        ensure_device_id_configured || exit 1
        shift
        service_heartbeat_forever "$@"
        ;;
    heartbeat_once)
        ensure_device_id_configured || exit 1
        shift
        service_heartbeat_once "$@"
        ;;
    listener)
        ensure_device_id_configured || exit 1
        shift
        service_listener_forever "$@"
        ;;
    dispatch)
        ensure_device_id_configured || exit 1
        shift
        service_dispatch "$@"
        ;;
    segment_worker)
        ensure_device_id_configured || exit 1
        shift
        service_segment_worker "$@"
        ;;
    *)
        echo "usage: sh app_service.sh {register|heartbeat|heartbeat_once|listener|dispatch|segment_worker}"
        exit 1
        ;;
esac
