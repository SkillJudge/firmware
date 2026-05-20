#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

ensure_runtime_files() {
    ensure_layout
    [ -f "$TIME_OFFSET_FILE" ] || echo 0 > "$TIME_OFFSET_FILE"
    [ -f "$SERVER_TIMESTAMP_FILE" ] || : > "$SERVER_TIMESTAMP_FILE"
}

runtime_read_value() {
    file="$1"
    default_value="$2"

    if [ -f "$file" ]; then
        value=$(cat "$file" 2>/dev/null)
        if [ -n "$value" ]; then
            printf '%s\n' "$value"
            return
        fi
    fi

    printf '%s\n' "$default_value"
}

runtime_write_value() {
    file="$1"
    value="$2"
    printf '%s\n' "$value" > "$file"
}

get_time_offset_ms() {
    runtime_read_value "$TIME_OFFSET_FILE" 0
}

current_now_ms() {
    base_ms=$(raw_now_ms)
    offset_ms=$(get_time_offset_ms)
    [ -n "$offset_ms" ] || offset_ms=0
    echo $((base_ms + offset_ms))
}

sync_time_from_timestamp_ms() {
    server_ms="$1"
    [ -n "$server_ms" ] || return 1

    base_ms=$(raw_now_ms)
    offset_ms=$((server_ms - base_ms))
    runtime_write_value "$TIME_OFFSET_FILE" "$offset_ms"
    runtime_write_value "$SERVER_TIMESTAMP_FILE" "$server_ms"
    log_info "time sync applied server_ms=$server_ms offset_ms=$offset_ms"
    return 0
}

save_runtime_ftp_config() {
    [ -n "$1" ] && runtime_write_value "$RUNTIME_FTP_HOST_FILE" "$1"
    [ -n "$2" ] && runtime_write_value "$RUNTIME_FTP_PORT_FILE" "$2"
    [ -n "$3" ] && runtime_write_value "$RUNTIME_FTP_USER_FILE" "$3"
    [ -n "$4" ] && runtime_write_value "$RUNTIME_FTP_PASS_FILE" "$4"
}

save_runtime_srs_config() {
    [ -n "$1" ] && runtime_write_value "$RUNTIME_SRS_HOST_FILE" "$1"
    [ -n "$2" ] && runtime_write_value "$RUNTIME_SRS_PORT_FILE" "$2"
    [ -n "$3" ] && runtime_write_value "$RUNTIME_SRS_USER_FILE" "$3"
    [ -n "$4" ] && runtime_write_value "$RUNTIME_SRS_PASS_FILE" "$4"
}

get_runtime_ftp_host() { runtime_read_value "$RUNTIME_FTP_HOST_FILE" "$FTP_HOST"; }
get_runtime_ftp_port() { runtime_read_value "$RUNTIME_FTP_PORT_FILE" "$FTP_PORT"; }
get_runtime_ftp_user() { runtime_read_value "$RUNTIME_FTP_USER_FILE" "$FTP_USER"; }
get_runtime_ftp_pass() { runtime_read_value "$RUNTIME_FTP_PASS_FILE" "$FTP_PASS"; }
get_runtime_srs_host() { runtime_read_value "$RUNTIME_SRS_HOST_FILE" "$SRS_HOST"; }
get_runtime_srs_port() { runtime_read_value "$RUNTIME_SRS_PORT_FILE" "$SRS_PORT"; }
get_runtime_srs_user() { runtime_read_value "$RUNTIME_SRS_USER_FILE" ""; }
get_runtime_srs_pass() { runtime_read_value "$RUNTIME_SRS_PASS_FILE" ""; }
