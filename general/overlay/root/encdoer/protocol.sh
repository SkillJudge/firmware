#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/mqtt.sh"

json_normalize() {
    printf '%s' "$1" | tr -d '\r\n'
}

json_require_jq() {
    require_command jq || return 1
}

json_get_path_raw() {
    json=$(json_normalize "$1")
    path="$2"
    json_require_jq || return 1

    printf '%s' "$json" | jq -r ".${path} | if . == null then empty else . end" 2>/dev/null
}

json_get_string() {
    json="$1"
    path="$2"

    raw_value=$(json_get_path_raw "$json" "$path")
    [ -n "$raw_value" ] || return 0
    printf '%s\n' "$raw_value"
}

json_get_number() {
    json="$1"
    path="$2"

    raw_value=$(json_get_path_raw "$json" "$path")
    [ -n "$raw_value" ] || return 0
    printf '%s' "$raw_value" | awk '
    {
        value = $0
        sub(/^[[:space:]]+/, "", value)
        sub(/[[:space:]]+$/, "", value)
        print value
    }'
}

json_get_bool() {
    json="$1"
    path="$2"

    raw_value=$(json_get_path_raw "$json" "$path")
    [ -n "$raw_value" ] || return 0
    printf '%s' "$raw_value" | awk '
    {
        value = $0
        sub(/^[[:space:]]+/, "", value)
        sub(/[[:space:]]+$/, "", value)
        print value
    }'
}

protocol_reset_context() {
    PROTO_ERROR=""
    PROTO_SENDER=""
    PROTO_SENDER_SUB=""
    PROTO_RECEIVER=""
    PROTO_RECEIVER_SUB=""
    PROTO_FLOW=""
    PROTO_ACTION=""
    PROTO_MSG_ID=""
    PROTO_MSG=""
    PROTO_COMMAND=""
    PROTO_REPLY_REQUIRED="false"
    PROTO_RECORD_ID=""
    PROTO_CAPTURE_ID=""
    PROTO_TASK_ID=""
    PROTO_STREAM_URL=""
    PROTO_DURATION=""
    PROTO_REASON=""
    PROTO_FILE_NAME=""
    PROTO_FILE_PATH=""
}

protocol_build_topic() {
    printf '%s/%s/%s/%s/%s/%s\n' "$1" "$2" "$3" "$4" "$5" "$6"
}

protocol_build_payload() {
    msg_id="$1"
    msg="$2"
    data_json="$3"
    json_require_jq || return 1
    [ -n "$data_json" ] || data_json="{}"
    jq -nc --argjson msgId "$msg_id" --arg msg "$msg" --argjson data "$data_json" '{msgId:$msgId,msg:$msg,data:$data}'
}

protocol_build_register_payload() {
    msg_id="$1"
    json_require_jq || return 1
    jq -nc --argjson msgId "$msg_id" --arg version "$DEVICE_VERSION" '{msgId:$msgId,msg:"register",data:{version:$version}}'
}

protocol_build_heartbeat_payload() {
    msg_id="$1"
    json_require_jq || return 1
    jq -nc \
        --argjson msgId "$msg_id" \
        --arg version "$DEVICE_VERSION" \
        --argjson is_idle "$(state_get_idle)" \
        --argjson is_recording "$(state_get_recording)" \
        --argjson is_publishing "$(state_get_publishing)" \
        --argjson is_charging "$(state_get_charging)" \
        --argjson signal "$(state_get_signal)" \
        --argjson battery "$(state_get_battery)" \
        '{msgId:$msgId,msg:"heartbeat",data:{is_idle:$is_idle,is_recording:$is_recording,is_publishing:$is_publishing,is_charging:$is_charging,version:$version,signal:$signal,battery:$battery}}'
}

protocol_parse_topic() {
    topic="$1"
    old_ifs=$IFS
    IFS='/'
    set -- $topic
    IFS=$old_ifs

    [ "$#" -eq 6 ] || return 1

    PROTO_SENDER="$1"
    PROTO_SENDER_SUB="$2"
    PROTO_RECEIVER="$3"
    PROTO_RECEIVER_SUB="$4"
    PROTO_FLOW="$5"
    PROTO_ACTION="$6"
    return 0
}

protocol_parse_command() {
    topic="$1"
    payload="$2"

    protocol_reset_context

    if ! protocol_parse_topic "$topic"; then
        PROTO_ERROR="invalid topic"
        return 1
    fi

    if [ "$PROTO_RECEIVER" != "encoder" ] || [ "$PROTO_RECEIVER_SUB" != "$DEVICE_ID" ]; then
        PROTO_COMMAND="ignore"
        return 0
    fi

    PROTO_MSG_ID=$(json_get_number "$payload" "msgId")
    PROTO_MSG=$(json_get_string "$payload" "msg")
    PROTO_RECORD_ID=$(json_get_string "$payload" "data.recordId")
    PROTO_CAPTURE_ID=$(json_get_string "$payload" "data.captureId")
    PROTO_TASK_ID=$(json_get_string "$payload" "data.taskId")
    PROTO_STREAM_URL=$(json_get_string "$payload" "data.streamUrl")
    PROTO_DURATION=$(json_get_number "$payload" "data.duration")
    PROTO_REASON=$(json_get_string "$payload" "data.reason")
    PROTO_FILE_NAME=$(json_get_string "$payload" "data.fileName")
    PROTO_FILE_PATH=$(json_get_string "$payload" "data.filePath")

    case "${PROTO_FLOW}/${PROTO_ACTION}/${PROTO_MSG}" in
        heartbeat/register_ack/registerAck|heartbeat/heartbeat_ack/heartbeatAck)
            PROTO_COMMAND="ignore"
            return 0
            ;;
    esac

    [ -n "$PROTO_MSG_ID" ] || {
        PROTO_ERROR="missing msgId"
        return 1
    }

    [ -n "$PROTO_MSG" ] || {
        PROTO_ERROR="missing msg"
        return 1
    }

    case "${PROTO_FLOW}/${PROTO_ACTION}/${PROTO_MSG}" in
        record/start_record/startRecord)
            PROTO_COMMAND="record_start"
            PROTO_REPLY_REQUIRED="true"
            ;;
        record/stop_record/stopRecord)
            PROTO_COMMAND="record_stop"
            PROTO_REPLY_REQUIRED="true"
            ;;
        capture/capture/capture)
            PROTO_COMMAND="capture_take"
            PROTO_REPLY_REQUIRED="true"
            ;;
        stream/start_stream/startStream)
            PROTO_COMMAND="stream_start"
            PROTO_REPLY_REQUIRED="true"
            ;;
        stream/stop_stream/stopStream)
            PROTO_COMMAND="stream_stop"
            PROTO_REPLY_REQUIRED="true"
            ;;
        play-audio/play_audio/playAudio)
            PROTO_COMMAND="audio_play"
            PROTO_REPLY_REQUIRED="true"
            ;;
        task/prepare_experiment_voice/prepareExperimentVoice)
            PROTO_COMMAND="task_prepare_voice"
            ;;
        task/start_stream/startStream)
            PROTO_COMMAND="task_stream_start"
            PROTO_REPLY_REQUIRED="true"
            ;;
        task/start_record/startRecord)
            PROTO_COMMAND="task_record_start"
            PROTO_REPLY_REQUIRED="true"
            ;;
        task/start_hand_recognition_voice/startHandRecognitionVoice)
            PROTO_COMMAND="task_hand_voice_start"
            ;;
        task/stop_hand_recognition_voice/stopHandRecognitionVoice)
            PROTO_COMMAND="task_hand_voice_stop"
            PROTO_REPLY_REQUIRED="true"
            ;;
        task/stop_record/stopRecord)
            PROTO_COMMAND="task_record_stop"
            PROTO_REPLY_REQUIRED="true"
            ;;
        task/reset_encoder/resetEncoder)
            PROTO_COMMAND="task_reset"
            PROTO_REPLY_REQUIRED="true"
            ;;
        *)
            PROTO_ERROR="unsupported request"
            return 1
            ;;
    esac

    return 0
}

protocol_publish_payload() {
    topic="$1"
    payload="$2"

    if [ "$MQTT_PAYLOAD_VERBOSE" = "true" ]; then
        log_info_tag "MQTT-PUB" "topic=$topic payload=$payload"
    else
        publish_msg=$(json_get_string "$payload" "msg")
        publish_status=$(json_get_string "$payload" "data.status")
        publish_code=$(json_get_number "$payload" "data.code")
        publish_record_id=$(json_get_string "$payload" "data.recordId")
        publish_stream_url=$(json_get_string "$payload" "data.streamUrl")
        log_info_tag "MQTT-PUB" "topic=$topic msg=${publish_msg:-unknown} status=${publish_status:-} code=${publish_code:-} record_id=${publish_record_id:-} stream_url=${publish_stream_url:-}"
    fi

    mqtt_pub_json "$topic" "$payload"
}

protocol_publish_segment_uploaded() {
    flow="$1"
    record_id="$2"
    file_name="$3"
    file_url="$4"
    file_size="$5"
    segment_no="$6"
    msg_id=$(next_msgid)

    case "$flow" in
        task)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "ctrlsrv" "0" "task" "segment_uploaded")
            data_json=$(jq -nc \
                --arg recordId "$record_id" \
                --arg fileUrl "$file_url" \
                --argjson fileSize "$file_size" \
                --argjson segmentNo "$segment_no" \
                '{recordId:$recordId,fileUrl:$fileUrl,fileSize:$fileSize,segmentNo:$segmentNo}')
            payload=$(protocol_build_payload "$msg_id" "segmentUploaded" "$data_json")
            ;;
        *)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "ctrlsrv" "0" "record" "segment_uploaded")
            data_json=$(jq -nc \
                --arg recordId "$record_id" \
                --arg fileName "$file_name" \
                --arg fileUrl "$file_url" \
                --argjson segmentNo "$segment_no" \
                '{recordId:$recordId,segmentNo:$segmentNo,fileName:$fileName,fileUrl:$fileUrl,status:"success"}')
            payload=$(protocol_build_payload "$msg_id" "segmentUploaded" "$data_json")
            ;;
    esac

    protocol_publish_payload "$topic" "$payload"
}

protocol_publish_command_result() {
    [ "$PROTO_REPLY_REQUIRED" = "true" ] || return 0

    case "$PROTO_COMMAND" in
        record_start)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "$PROTO_SENDER" "$PROTO_SENDER_SUB" "record" "start_record_ack")
            data_json=$(jq -nc \
                --argjson replyTo "$PROTO_MSG_ID" \
                --arg recordId "$RESULT_RECORD_ID" \
                --arg status "$RESULT_STATUS" \
                '{replyTo:$replyTo,recordId:$recordId,status:$status}')
            payload=$(protocol_build_payload "$PROTO_MSG_ID" "startRecordAck" "$data_json")
            ;;
        record_stop)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "$PROTO_SENDER" "$PROTO_SENDER_SUB" "record" "stop_record_ack")
            data_json=$(jq -nc \
                --argjson replyTo "$PROTO_MSG_ID" \
                --arg recordId "$RESULT_RECORD_ID" \
                --arg lastFile "$RESULT_FILE_NAME" \
                --arg status "$RESULT_STATUS" \
                '{replyTo:$replyTo,recordId:$recordId,lastFile:$lastFile,status:$status}')
            payload=$(protocol_build_payload "$PROTO_MSG_ID" "stopRecordAck" "$data_json")
            ;;
        capture_take)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "$PROTO_SENDER" "$PROTO_SENDER_SUB" "capture" "capture_ack")
            data_json=$(jq -nc \
                --argjson replyTo "$PROTO_MSG_ID" \
                --arg captureId "$RESULT_CAPTURE_ID" \
                --arg fileName "$RESULT_FILE_NAME" \
                --arg fileUrl "$RESULT_FILE_URL" \
                --arg status "$RESULT_STATUS" \
                '{replyTo:$replyTo,captureId:$captureId,fileName:$fileName,fileUrl:$fileUrl,status:$status}')
            payload=$(protocol_build_payload "$PROTO_MSG_ID" "captureAck" "$data_json")
            ;;
        stream_start)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "$PROTO_SENDER" "$PROTO_SENDER_SUB" "stream" "start_stream_ack")
            data_json=$(jq -nc \
                --argjson replyTo "$PROTO_MSG_ID" \
                --arg streamUrl "$RESULT_STREAM_URL" \
                --arg status "$RESULT_STATUS" \
                '{replyTo:$replyTo,streamUrl:$streamUrl,status:$status}')
            payload=$(protocol_build_payload "$PROTO_MSG_ID" "startStreamAck" "$data_json")
            ;;
        stream_stop)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "$PROTO_SENDER" "$PROTO_SENDER_SUB" "stream" "stop_stream_ack")
            data_json=$(jq -nc \
                --argjson replyTo "$PROTO_MSG_ID" \
                --arg status "$RESULT_STATUS" \
                '{replyTo:$replyTo,status:$status}')
            payload=$(protocol_build_payload "$PROTO_MSG_ID" "stopStreamAck" "$data_json")
            ;;
        audio_play)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "$PROTO_SENDER" "$PROTO_SENDER_SUB" "play-audio" "play_audio_ack")
            data_json=$(jq -nc \
                --argjson replyTo "$PROTO_MSG_ID" \
                --arg fileName "$RESULT_AUDIO_FILE_NAME" \
                --arg status "$RESULT_STATUS" \
                '{replyTo:$replyTo,fileName:$fileName,status:$status}')
            payload=$(protocol_build_payload "$PROTO_MSG_ID" "playAudioAck" "$data_json")
            ;;
        task_stream_start)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "$PROTO_SENDER" "$PROTO_SENDER_SUB" "task" "start_stream_ack")
            data_json=$(jq -nc \
                --argjson replyTo "$PROTO_MSG_ID" \
                --arg taskId "$RESULT_TASK_ID" \
                --argjson code "$RESULT_CODE" \
                '{replyTo:$replyTo,taskId:$taskId,code:$code}')
            payload=$(protocol_build_payload "$PROTO_MSG_ID" "startStreamAck" "$data_json")
            ;;
        task_record_start)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "$PROTO_SENDER" "$PROTO_SENDER_SUB" "task" "start_record_ack")
            data_json=$(jq -nc \
                --argjson replyTo "$PROTO_MSG_ID" \
                --arg taskId "$RESULT_TASK_ID" \
                --arg recordId "$RESULT_RECORD_ID" \
                --argjson code "$RESULT_CODE" \
                '{replyTo:$replyTo,taskId:$taskId,recordId:$recordId,code:$code}')
            payload=$(protocol_build_payload "$PROTO_MSG_ID" "startRecordAck" "$data_json")
            ;;
        task_record_stop)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "$PROTO_SENDER" "$PROTO_SENDER_SUB" "task" "last_segment_uploaded")
            data_json=$(jq -nc \
                --arg recordId "$RESULT_RECORD_ID" \
                --argjson fileSize "$RESULT_FILE_SIZE" \
                --arg fileUrl "$RESULT_FILE_URL" \
                --argjson segmentNo "$RESULT_SEGMENT_NO" \
                '{recordId:$recordId,fileSize:$fileSize,fileUrl:$fileUrl,segmentNo:$segmentNo}')
            payload=$(protocol_build_payload "$PROTO_MSG_ID" "lastSegmentUploaded" "$data_json")
            ;;
        task_hand_voice_stop)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "$PROTO_SENDER" "$PROTO_SENDER_SUB" "task" "stop_hand_recognition_voice_ack")
            data_json=$(jq -nc \
                --argjson replyTo "$PROTO_MSG_ID" \
                --arg recordId "$RESULT_RECORD_ID" \
                --argjson code "$RESULT_CODE" \
                '{replyTo:$replyTo,recordId:$recordId,code:$code}')
            payload=$(protocol_build_payload "$PROTO_MSG_ID" "stopHandRecognitionVoiceAck" "$data_json")
            ;;
        task_reset)
            topic=$(protocol_build_topic "encoder" "$DEVICE_ID" "$PROTO_SENDER" "$PROTO_SENDER_SUB" "task" "reset_encoder_ack")
            data_json=$(jq -nc \
                --argjson replyTo "$PROTO_MSG_ID" \
                --arg taskId "$RESULT_TASK_ID" \
                --argjson code "$RESULT_CODE" \
                '{replyTo:$replyTo,taskId:$taskId,code:$code}')
            payload=$(protocol_build_payload "$PROTO_MSG_ID" "resetEncoderAck" "$data_json")
            ;;
        *)
            log_warn "no publish rule for command=$PROTO_COMMAND"
            return 0
            ;;
    esac

    protocol_publish_payload "$topic" "$payload"
}
