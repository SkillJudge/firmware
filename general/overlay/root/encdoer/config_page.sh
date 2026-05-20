#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
CONFIG_FILE="$SCRIPT_DIR/config.sh"
. "$SCRIPT_DIR/state.sh"
. "$SCRIPT_DIR/runtime.sh"

is_editable_key() {
    key="$1"
    for item in $CONFIG_EDITABLE_KEYS; do
        [ "$item" = "$key" ] && return 0
    done
    return 1
}

config_replace_value() {
    key="$1"
    value="$2"

    if ! is_editable_key "$key"; then
        log_error "config key not editable: $key"
        return 1
    fi

    escaped_value=$(printf '%s' "$value" | sed 's/[\/&]/\\&/g')
    sed "s|^${key}=.*$|${key}=\"${escaped_value}\"|" "$CONFIG_FILE" > "${CONFIG_FILE}.tmp" || return 1
    mv "${CONFIG_FILE}.tmp" "$CONFIG_FILE"
    log_info "config updated key=$key value=$value"
    return 0
}

show_page() {
    ensure_layout
    state_init
    ensure_runtime_files
    print_title "$CONFIG_PAGE_TITLE"

    cat <<EOF
[Version]
DEVICE_VERSION=$DEVICE_VERSION
DEVICE_ID=$DEVICE_ID
DEVICE_ID_DEFAULT=$DEVICE_ID_DEFAULT
DEVICE_ID_SOURCE=$DEVICE_ID_SOURCE
DEVICE_ID_CONFIGURED=$DEVICE_ID_CONFIGURED

[MQTT]
MQTT_HOST=$MQTT_HOST
MQTT_PORT=$MQTT_PORT
MQTT_USER=$MQTT_USER
MQTT_QOS=$MQTT_QOS
MQTT_SUBSCRIBE_TOPIC=$MQTT_SUBSCRIBE_TOPIC
MQTT_REGISTER_TOPIC=$MQTT_REGISTER_TOPIC
MQTT_HEARTBEAT_TOPIC=$MQTT_HEARTBEAT_TOPIC

[Service]
HEARTBEAT_INTERVAL_SEC=$HEARTBEAT_INTERVAL_SEC
REGISTER_ACK_TIMEOUT_SEC=$REGISTER_ACK_TIMEOUT_SEC
REGISTER_RETRY_INTERVAL_SEC=$REGISTER_RETRY_INTERVAL_SEC
SEGMENT_SCAN_INTERVAL_SEC=$SEGMENT_SCAN_INTERVAL_SEC
SEGMENT_STABLE_SEC=$SEGMENT_STABLE_SEC
RECORD_FINALIZE_WAIT_SEC=$RECORD_FINALIZE_WAIT_SEC
CURL_CONNECT_TIMEOUT_SEC=$CURL_CONNECT_TIMEOUT_SEC
CURL_UPLOAD_MAX_TIME_SEC=$CURL_UPLOAD_MAX_TIME_SEC
RECORD_FILE_TIME_FORMAT=$RECORD_FILE_TIME_FORMAT
RECORD_FILE_NAME_TEMPLATE=$RECORD_FILE_NAME_TEMPLATE
MAIN_STREAM_ENABLED=$MAIN_STREAM_ENABLED
MAIN_STREAM_CODEC=$MAIN_STREAM_CODEC
MAIN_STREAM_SIZE=$MAIN_STREAM_SIZE
MAIN_STREAM_FPS=$MAIN_STREAM_FPS
MAIN_STREAM_BITRATE=$MAIN_STREAM_BITRATE
SUB_STREAM_ENABLED=$SUB_STREAM_ENABLED
SUB_STREAM_CODEC=$SUB_STREAM_CODEC
SUB_STREAM_SIZE=$SUB_STREAM_SIZE
SUB_STREAM_FPS=$SUB_STREAM_FPS
SUB_STREAM_BITRATE=$SUB_STREAM_BITRATE

[Media]
FTP_HOST=$FTP_HOST
FTP_PORT=$FTP_PORT
FTP_USER=$FTP_USER
SRS_HOST=$SRS_HOST
SRS_PORT=$SRS_PORT
SRS_APP=$SRS_APP
SRS_STREAM_PREFIX=$SRS_STREAM_PREFIX
STREAM_PUSH_URL=$STREAM_PUSH_URL
STREAM_SUBSTREAM=$STREAM_SUBSTREAM
CAPTURE_SNAPSHOT_URL=$CAPTURE_SNAPSHOT_URL
CAPTURE_LOCAL_DIR=$CAPTURE_LOCAL_DIR
RECORD_NAMED_LOCAL_DIR=$RECORD_NAMED_LOCAL_DIR
RECORD_SEARCH_ROOT=$RECORD_SEARCH_ROOT
STREAM_CONFIG_FILE=$STREAM_CONFIG_FILE
STREAM_CONFIG_BACKUP=$STREAM_CONFIG_BACKUP
RECORD_PATH=$RECORD_PATH
RECORD_SPLIT=$RECORD_SPLIT
RECORD_MAX_USAGE=$RECORD_MAX_USAGE
RECORD_SUBSTREAM=$RECORD_SUBSTREAM
RECORD_REMOTE_ROOT=$RECORD_REMOTE_ROOT

[Log]
LOG_VERBOSE=$LOG_VERBOSE
CONFIG_COMMAND_VERBOSE=$CONFIG_COMMAND_VERBOSE
MQTT_PAYLOAD_VERBOSE=$MQTT_PAYLOAD_VERBOSE
HEARTBEAT_LOG_VERBOSE=$HEARTBEAT_LOG_VERBOSE

[Runtime Media]
RUNTIME_FTP_HOST=$(get_runtime_ftp_host)
RUNTIME_FTP_PORT=$(get_runtime_ftp_port)
RUNTIME_FTP_USER=$(get_runtime_ftp_user)
RUNTIME_SRS_HOST=$(get_runtime_srs_host)
RUNTIME_SRS_PORT=$(get_runtime_srs_port)
CURRENT_STREAM_URL=$(state_get_current_stream_url)
SERVER_TIMESTAMP_MS=$(runtime_read_value "$SERVER_TIMESTAMP_FILE" "")
TIME_OFFSET_MS=$(get_time_offset_ms)

[Runtime State]
$(state_dump)

[Usage]
sh $APP_HOME/config_page.sh show
sh $APP_HOME/config_page.sh get MQTT_HOST
sh $APP_HOME/config_page.sh set MQTT_HOST 127.0.0.1
sh $APP_HOME/device_id.sh check
EOF
}

case "$1" in
    show|"")
        show_page
        ;;
    get)
        key="$2"
        [ -n "$key" ] || {
            echo "usage: sh config_page.sh get KEY"
            exit 1
        }
        case "$key" in
            DEVICE_ID|ENCODER_DEVICE_ID)
                printf 'DEVICE_ID="%s"\n' "$DEVICE_ID"
                exit 0
                ;;
            DEVICE_ID_DEFAULT)
                printf 'DEVICE_ID_DEFAULT="%s"\n' "$DEVICE_ID_DEFAULT"
                exit 0
                ;;
            DEVICE_ID_SOURCE)
                printf 'DEVICE_ID_SOURCE="%s"\n' "$DEVICE_ID_SOURCE"
                exit 0
                ;;
        esac
        grep "^${key}=" "$CONFIG_FILE"
        ;;
    set)
        key="$2"
        value="$3"
        [ -n "$key" ] && [ -n "$value" ] || {
            echo "usage: sh config_page.sh set KEY VALUE"
            exit 1
        }
        config_replace_value "$key" "$value"
        ;;
    *)
        echo "usage: sh config_page.sh {show|get KEY|set KEY VALUE}"
        exit 1
        ;;
esac
