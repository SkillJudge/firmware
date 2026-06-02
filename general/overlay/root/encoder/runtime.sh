#!/bin/sh

# 运行时配置层。
# 云端 registerAck 下发的 FTP/SRS 参数和服务器时间偏移会写在 runtime/state 下，优先级高于 config.sh 默认值。
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

ensure_runtime_files() {
    # 初始化运行时文件。只补缺省文件，不覆盖云端已经下发的值。
    ensure_layout
    [ -f "$TIME_OFFSET_FILE" ] || echo 0 > "$TIME_OFFSET_FILE"
    [ -f "$SERVER_TIMESTAMP_FILE" ] || : > "$SERVER_TIMESTAMP_FILE"
}

runtime_read_value() {
    # 读取运行时值，文件为空或不存在时返回调用方给出的默认值。
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
    # 写入运行时值。这里不做日志，避免注册时泄露账号密码。
    file="$1"
    value="$2"
    printf '%s\n' "$value" > "$file"
}

get_time_offset_ms() {
    # 云端时间和板端本地时间之间的毫秒偏移。
    runtime_read_value "$TIME_OFFSET_FILE" 0
}

current_now_ms() {
    # 当前业务时间戳：本机时间 + 云端下发偏移。
    base_ms=$(raw_now_ms)
    offset_ms=$(get_time_offset_ms)
    [ -n "$offset_ms" ] || offset_ms=0
    echo $((base_ms + offset_ms))
}

sync_time_from_timestamp_ms() {
    # 云端 registerAck 中的 timestamp 用来计算偏移，不直接修改系统时间。
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
    # 保存云端下发的 FTP 参数。空字段不覆盖本地已有值。
    [ -n "$1" ] && runtime_write_value "$RUNTIME_FTP_HOST_FILE" "$1"
    [ -n "$2" ] && runtime_write_value "$RUNTIME_FTP_PORT_FILE" "$2"
    [ -n "$3" ] && runtime_write_value "$RUNTIME_FTP_USER_FILE" "$3"
    [ -n "$4" ] && runtime_write_value "$RUNTIME_FTP_PASS_FILE" "$4"
}

save_runtime_srs_config() {
    # 保存云端下发的 SRS 参数，供后续自动拼接推流地址。
    [ -n "$1" ] && runtime_write_value "$RUNTIME_SRS_HOST_FILE" "$1"
    [ -n "$2" ] && runtime_write_value "$RUNTIME_SRS_PORT_FILE" "$2"
    [ -n "$3" ] && runtime_write_value "$RUNTIME_SRS_USER_FILE" "$3"
    [ -n "$4" ] && runtime_write_value "$RUNTIME_SRS_PASS_FILE" "$4"
}

# 运行时配置读取接口：runtime 文件存在且非空时优先，否则回退到 config.sh 默认配置。
get_runtime_ftp_host() { runtime_read_value "$RUNTIME_FTP_HOST_FILE" "$FTP_HOST"; }
get_runtime_ftp_port() { runtime_read_value "$RUNTIME_FTP_PORT_FILE" "$FTP_PORT"; }
get_runtime_ftp_user() { runtime_read_value "$RUNTIME_FTP_USER_FILE" "$FTP_USER"; }
get_runtime_ftp_pass() { runtime_read_value "$RUNTIME_FTP_PASS_FILE" "$FTP_PASS"; }
get_runtime_srs_host() { runtime_read_value "$RUNTIME_SRS_HOST_FILE" "$SRS_HOST"; }
get_runtime_srs_port() { runtime_read_value "$RUNTIME_SRS_PORT_FILE" "$SRS_PORT"; }
get_runtime_srs_user() { runtime_read_value "$RUNTIME_SRS_USER_FILE" ""; }
get_runtime_srs_pass() { runtime_read_value "$RUNTIME_SRS_PASS_FILE" ""; }
