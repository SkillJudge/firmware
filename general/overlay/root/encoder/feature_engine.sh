#!/bin/sh

# 功能引擎层。
# 这一层不直接订阅 MQTT，只接收已经解析好的命令参数，并完成推流、录像、分片上传、抓拍和重置。
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/protocol.sh"
. "$SCRIPT_DIR/state.sh"
. "$SCRIPT_DIR/runtime.sh"
. "$SCRIPT_DIR/led.sh"

feature_result_reset() {
    # 每次处理命令前重置结果上下文，protocol.sh 会根据这些 RESULT_* 变量组装 ACK。
    RESULT_OK="false"
    RESULT_CODE="-1"
    RESULT_STATUS="fail"
    RESULT_RECORD_ID=""
    RESULT_STREAM_URL=""
    RESULT_FILE_NAME=""
    RESULT_FILE_URL=""
    RESULT_FILE_SIZE="0"
    RESULT_SEGMENT_NO="0"
    RESULT_CAPTURE_ID=""
    RESULT_TASK_ID=""
    RESULT_AUDIO_FILE_NAME=""
}

feature_result_success() {
    # 标记本次业务动作成功，并写入协议回包需要的状态值。
    RESULT_OK="true"
    RESULT_CODE="${1:-0}"
    RESULT_STATUS="$2"
}

feature_result_failure() {
    # 标记失败结果。调用方仍会尝试发布 ACK，让控制端知道失败状态。
    RESULT_OK="false"
    RESULT_CODE="${1:--1}"
    RESULT_STATUS="$2"
}

build_stream_name() {
    # 推流名默认使用 stream_${DEVICE_ID}，也可以通过 SRS_STREAM_PREFIX 覆盖。
    if [ -n "$SRS_STREAM_PREFIX" ]; then
        printf '%s\n' "$SRS_STREAM_PREFIX"
    else
        printf 'stream_%s\n' "$DEVICE_ID"
    fi
}

build_stream_url_with_name() {
    # 控制端可能下发 rtmp://host/app/xxx，这里统一替换最后一级为本设备的 stream_name。
    url="$1"
    stream_name=$(build_stream_name)
    base_url=${url%/}

    case "$base_url" in
        *://*/*/*)
            printf '%s/%s\n' "${base_url%/*}" "$stream_name"
            ;;
        *)
            printf '%s/%s\n' "$base_url" "$stream_name"
            ;;
    esac
}

build_stream_url() {
    # 推流 URL 优先级：命令下发 streamUrl -> 本地 STREAM_PUSH_URL -> registerAck/runtime SRS 配置。
    requested_url="$1"
    if [ -n "$requested_url" ]; then
        build_stream_url_with_name "$requested_url"
        return
    fi

    if [ -n "$STREAM_PUSH_URL" ]; then
        build_stream_url_with_name "$STREAM_PUSH_URL"
        return
    fi

    runtime_srs_host=$(get_runtime_srs_host)
    runtime_srs_port=$(get_runtime_srs_port)
    [ -n "$runtime_srs_host" ] || runtime_srs_host="$SRS_HOST"
    [ -n "$runtime_srs_port" ] || runtime_srs_port="$SRS_PORT"

    if [ -z "$runtime_srs_host" ] || [ -z "$runtime_srs_port" ] || [ -z "$SRS_APP" ]; then
        log_error "stream url missing: provide data.streamUrl in MQTT command or srs.host/srs.port in registerAck"
        return 1
    fi

    printf 'rtmp://%s:%s/%s/%s\n' "$runtime_srs_host" "$runtime_srs_port" "$SRS_APP" "$(build_stream_name)"
}

build_ftp_url() {
    # 上传时使用完整 FTP URL，包含认证信息；不要直接写入普通日志。
    relative_path=${1#/}
    runtime_ftp_host=$(get_runtime_ftp_host)
    runtime_ftp_port=$(get_runtime_ftp_port)
    runtime_ftp_user=$(get_runtime_ftp_user)
    runtime_ftp_pass=$(get_runtime_ftp_pass)
    printf 'ftp://%s:%s@%s:%s/%s\n' "$runtime_ftp_user" "$runtime_ftp_pass" "$runtime_ftp_host" "$runtime_ftp_port" "$relative_path"
}

build_ftp_log_url() {
    # 日志中的 FTP URL 会隐藏密码，避免现场日志泄露敏感信息。
    relative_path=${1#/}
    runtime_ftp_host=$(get_runtime_ftp_host)
    runtime_ftp_port=$(get_runtime_ftp_port)
    runtime_ftp_user=$(get_runtime_ftp_user)
    printf 'ftp://%s:***@%s:%s/%s\n' "$runtime_ftp_user" "$runtime_ftp_host" "$runtime_ftp_port" "$relative_path"
}

build_ftp_report_url() {
    # 上报给控制端的文件 URL 不带用户名密码，只描述文件可访问位置。
    relative_path=${1#/}
    runtime_ftp_host=$(get_runtime_ftp_host)
    runtime_ftp_port=$(get_runtime_ftp_port)

    if [ -n "$runtime_ftp_port" ] && [ "$runtime_ftp_port" != "21" ]; then
        printf 'ftp://%s:%s/%s\n' "$runtime_ftp_host" "$runtime_ftp_port" "$relative_path"
    else
        printf 'ftp://%s/%s\n' "$runtime_ftp_host" "$relative_path"
    fi
}

build_record_remote_path() {
    # 录像上传路径。当前需求是 upload/<device_id>/<record_id>/<file_name>。
    record_id="$1"
    file_name="$2"
    remote_root="${RECORD_REMOTE_ROOT:-upload}"
    printf '%s/%s/%s/%s\n' "$remote_root" "$DEVICE_ID" "$record_id" "$file_name"
}

build_record_output_name() {
    # 根据模板生成上传文件名，保留同一次录像的 session_time，避免分片文件时间戳漂移。
    segment_no="$1"
    task_id="$2"
    session_time=$(state_get_record_session_time)
    safe_device_id=$(sanitize_name_part "$DEVICE_ID")
    safe_task_id=$(sanitize_name_part "${task_id:-manual}")
    safe_segment_no=$(sanitize_name_part "$segment_no")
    template="$RECORD_FILE_NAME_TEMPLATE"

    [ -n "$session_time" ] || session_time=$(now_with_format "$RECORD_FILE_TIME_FORMAT")
    safe_timestamp=$(sanitize_name_part "$session_time")

    printf '%s' "$template" | sed \
        -e "s/{device_id}/$safe_device_id/g" \
        -e "s/{task_id}/$safe_task_id/g" \
        -e "s/{timestamp}/$safe_timestamp/g" \
        -e "s/{segment_no}/$safe_segment_no/g"
}

prepare_record_named_local_file() {
    # 本地文件名由 Majestic 生成且不可控，上传前先在 runtime/named_records 下创建标准命名入口。
    source_file="$1"
    output_name="$2"
    named_file="${RECORD_NAMED_LOCAL_DIR}/${output_name}"

    ensure_layout
    rm -f "$named_file"

    if ln -s "$source_file" "$named_file" 2>/dev/null; then
        printf '%s\n' "$named_file"
        return 0
    fi

    if cp "$source_file" "$named_file" 2>/dev/null; then
        printf '%s\n' "$named_file"
        return 0
    fi

    log_warn "named local file create failed source_file=$source_file named_file=$named_file, upload original file"
    printf '%s\n' "$source_file"
    return 1
}

build_capture_remote_path() {
    # 抓拍文件单独放在 capture 目录，避免和录像分片混在一起。
    capture_id="$1"
    printf 'capture/%s/%s.jpg\n' "$DEVICE_ID" "$capture_id"
}

ftp_upload_file() {
    # 统一 FTP 上传封装：检查本地文件、读取 runtime FTP 配置、上传并输出精简日志。
    local_file="$1"
    remote_path="$2"

    ensure_layout

    if [ ! -f "$local_file" ]; then
        log_error "ftp upload skipped local_file_missing local_file=$local_file remote_path=$remote_path"
        return 1
    fi

    runtime_ftp_host=$(get_runtime_ftp_host)
    runtime_ftp_port=$(get_runtime_ftp_port)
    runtime_ftp_user=$(get_runtime_ftp_user)
    runtime_ftp_pass=$(get_runtime_ftp_pass)

    if [ -z "$runtime_ftp_host" ] || [ -z "$runtime_ftp_port" ] || [ -z "$runtime_ftp_user" ] || [ -z "$runtime_ftp_pass" ]; then
        log_error "ftp upload skipped ftp_config_missing host=${runtime_ftp_host:-empty} port=${runtime_ftp_port:-empty} user=${runtime_ftp_user:-empty} pass_configured=$([ -n "$runtime_ftp_pass" ] && printf true || printf false) remote_path=$remote_path"
        return 1
    fi

    ftp_url=$(build_ftp_url "$remote_path")
    ftp_log_url=$(build_ftp_log_url "$remote_path")
    ftp_report_url=$(build_ftp_report_url "$remote_path")
    local_size=$(file_size_bytes "$local_file")
    [ -n "$local_size" ] || local_size=0

    if [ "$runtime_ftp_port" = "22" ]; then
        log_warn "ftp upload port looks like ssh/sftp port but uploader is ftp protocol host=$runtime_ftp_host port=$runtime_ftp_port url=$ftp_log_url"
    fi

    upload_start_ts=$(raw_now_sec)
    err_file="${TMP_DIR}/curl_upload_$$.err"
    rm -f "$err_file"

    log_debug "ftp upload begin protocol=ftp host=$runtime_ftp_host port=$runtime_ftp_port user=$runtime_ftp_user local_file=$local_file local_size=$local_size remote_path=$remote_path url=$ftp_log_url connect_timeout=${CURL_CONNECT_TIMEOUT_SEC}s max_time=${CURL_UPLOAD_MAX_TIME_SEC}s"
    led_upload_begin
    upload_led_token="$LED_UPLOAD_TOKEN"
    curl -sS --ftp-pasv --ftp-create-dirs \
        --connect-timeout "$CURL_CONNECT_TIMEOUT_SEC" \
        --max-time "$CURL_UPLOAD_MAX_TIME_SEC" \
        -T "$local_file" "$ftp_url" 2>"$err_file"
    rc=$?
    led_upload_end "$upload_led_token"
    upload_end_ts=$(raw_now_sec)
    elapsed_sec=$((upload_end_ts - upload_start_ts))
    curl_error=$(tr '\r\n' '  ' < "$err_file" 2>/dev/null)
    rm -f "$err_file"

    if [ "$rc" = "0" ]; then
        log_debug "ftp upload success protocol=ftp host=$runtime_ftp_host port=$runtime_ftp_port user=$runtime_ftp_user local_file=$local_file local_size=$local_size remote_path=$remote_path file_url=$ftp_report_url elapsed=${elapsed_sec}s rc=$rc"
        return 0
    fi

    [ -n "$curl_error" ] || curl_error="empty"
    log_error "ftp upload failed protocol=ftp host=$runtime_ftp_host port=$runtime_ftp_port user=$runtime_ftp_user local_file=$local_file local_size=$local_size remote_path=$remote_path url=$ftp_log_url elapsed=${elapsed_sec}s rc=$rc curl_error=$curl_error"
    return "$rc"
}

feature_yaml_set() {
    label="$1"
    key="$2"
    value="$3"

    require_command yaml-cli || return 1
    quoted_config_file=$(shell_quote "$STREAM_CONFIG_FILE")
    run_config_command "$label" "yaml-cli -i $quoted_config_file -s $key $value"
}

feature_cli_set_bool() {
    # 写入 Majestic 布尔配置，底层依赖板子上的 yaml-cli 命令。
    label="$1"
    key="$2"
    value="$3"
    feature_yaml_set "$label" "$key" "$value"
}

feature_cli_set_number() {
    # 写入 Majestic 数字配置。
    label="$1"
    key="$2"
    value="$3"
    feature_yaml_set "$label" "$key" "$value"
}

feature_cli_set_string() {
    # 字符串值需要先做 shell quote，避免 URL、路径中的特殊字符破坏命令。
    label="$1"
    key="$2"
    value="$3"
    quoted_value=$(shell_quote "$value")
    feature_yaml_set "$label" "$key" "$quoted_value"
}

feature_cli_set_optional_string() {
    # 空值表示保持当前板端配置，不主动写入。
    label="$1"
    key="$2"
    value="$3"
    [ -n "$value" ] || return 0
    feature_cli_set_string "$label" "$key" "$value"
}

feature_cli_set_optional_number() {
    # 可选数字字段同样跳过空值，便于只配置分辨率/帧率而不固定码率。
    label="$1"
    key="$2"
    value="$3"
    [ -n "$value" ] || return 0
    feature_cli_set_number "$label" "$key" "$value"
}

feature_media_apply_video_profile() {
    # 开始推流或录像前统一写入主码流/子码流配置。
    log_debug "apply video profile main_enabled=$MAIN_STREAM_ENABLED main_size=$MAIN_STREAM_SIZE main_fps=$MAIN_STREAM_FPS sub_enabled=$SUB_STREAM_ENABLED sub_size=$SUB_STREAM_SIZE sub_fps=$SUB_STREAM_FPS"

    feature_cli_set_bool "video0_enabled" ".video0.enabled" "$MAIN_STREAM_ENABLED" || return 1
    feature_cli_set_optional_string "video0_codec" ".video0.codec" "$MAIN_STREAM_CODEC" || return 1
    feature_cli_set_optional_string "video0_size" ".video0.size" "$MAIN_STREAM_SIZE" || return 1
    feature_cli_set_optional_number "video0_fps" ".video0.fps" "$MAIN_STREAM_FPS" || return 1
    feature_cli_set_optional_number "video0_bitrate" ".video0.bitrate" "$MAIN_STREAM_BITRATE" || return 1

    feature_cli_set_bool "video1_enabled" ".video1.enabled" "$SUB_STREAM_ENABLED" || return 1
    feature_cli_set_optional_string "video1_codec" ".video1.codec" "$SUB_STREAM_CODEC" || return 1
    feature_cli_set_optional_string "video1_size" ".video1.size" "$SUB_STREAM_SIZE" || return 1
    feature_cli_set_optional_number "video1_fps" ".video1.fps" "$SUB_STREAM_FPS" || return 1
    feature_cli_set_optional_number "video1_bitrate" ".video1.bitrate" "$SUB_STREAM_BITRATE" || return 1
}

feature_media_disable_all_video() {
    # 按 V-2.5 需求，停止推流或停止录像时同时关闭 video0/video1。
    log_info "disable all video streams video0=false video1=false"
    feature_cli_set_bool "video0_enabled" ".video0.enabled" false || return 1
    feature_cli_set_bool "video1_enabled" ".video1.enabled" false || return 1
}

feature_stream_apply_outgoing_profile() {
    # 写入 outgoing 推流配置并启用推流。reload 后 majestic 才会真正生效。
    stream_url="$1"

    log_debug "apply outgoing profile stream_url=$stream_url substream=$STREAM_SUBSTREAM"
    feature_cli_set_string "outgoing_server" ".outgoing.server" "$stream_url" || return 1
    feature_cli_set_bool "outgoing_substream" ".outgoing.substream" "$STREAM_SUBSTREAM" || return 1
    feature_cli_set_bool "outgoing_enabled" ".outgoing.enabled" true || return 1
}

feature_stream_disable_outgoing() {
    # 关闭 outgoing 并清空 server，防止下次启动前仍保留旧推流地址。
    feature_cli_set_string "outgoing_server" ".outgoing.server" "" || return 1
    feature_cli_set_bool "outgoing_enabled" ".outgoing.enabled" false
}

feature_record_apply_profile() {
    # 写入录像配置。RECORD_PATH 是 Majestic 本地落盘路径，远端 FTP 路径由 build_record_remote_path 决定。
    log_info "apply record profile path=$RECORD_PATH split=$RECORD_SPLIT maxUsage=$RECORD_MAX_USAGE substream=$RECORD_SUBSTREAM"
    feature_cli_set_string "records_path" ".records.path" "$RECORD_PATH" || return 1
    feature_cli_set_number "records_split" ".records.split" "$RECORD_SPLIT" || return 1
    feature_cli_set_number "records_maxUsage" ".records.maxUsage" "$RECORD_MAX_USAGE" || return 1
    feature_cli_set_bool "records_substream" ".records.substream" "$RECORD_SUBSTREAM" || return 1
}

feature_record_enable() {
    # 单独拆分 enable/disable，方便 stop/reset 时复用。
    feature_cli_set_bool "records_enabled" ".records.enabled" true
}

feature_record_disable() {
    # 关闭录像后仍需 reload，调用方负责串联 reload 和最终上传。
    feature_cli_set_bool "records_enabled" ".records.enabled" false
}

feature_reload_stream_service() {
    # 默认发送 HUP；如果 Majestic 已经退出，则自动清理旧 PID 文件并恢复系统服务。
    stream_service_reload_or_recover "stream_reload"
}

feature_stream_start() {
    # 普通 stream/start_stream 和 task/start_stream 共用本流程，只是 mode 影响回包格式。
    mode="$1"
    requested_stream_url="$2"
    duration="$3"
    task_id="$4"

    ensure_layout
    state_init
    ensure_runtime_files
    feature_result_reset

    push_url=$(build_stream_url "$requested_stream_url")
    RESULT_STREAM_URL="$push_url"
    RESULT_TASK_ID="${task_id:-$(state_get_current_task_id)}"

    if [ -z "$push_url" ]; then
        feature_result_failure -1 "fail"
        log_error "stream start failed mode=$mode reason=empty_stream_url"
        return 1
    fi

    log_info "stream start request mode=$mode stream_url=$push_url"

    if [ "$(state_get_publishing)" = "true" ] && [ "$(state_get_current_stream_url)" = "$push_url" ]; then
        log_warn "stream already running on requested url, forcing reload to refresh connection"
    fi

    if feature_media_apply_video_profile && feature_stream_apply_outgoing_profile "$push_url" && feature_reload_stream_service; then
        sleep 3
        state_set_publishing true
        state_set_current_stream_url "$push_url"
        [ -n "$task_id" ] && state_set_current_task_id "$task_id"
        state_recompute_idle
        feature_result_success 0 "streaming"
        log_info "stream start success mode=$mode stream_url=$push_url"
        led_schedule_business_sync
        return 0
    fi

    feature_result_failure -1 "fail"
    log_error "stream start failed mode=$mode stream_url=$push_url"
    return 1
}

feature_stream_stop() {
    # 停止推流时关闭 outgoing，同时关闭 video0/video1，满足现场释放视频链路的要求。
    mode="$1"
    reason="$2"

    ensure_layout
    state_init
    ensure_runtime_files
    feature_result_reset

    log_info "stream stop request mode=$mode reason=$reason"

    if feature_stream_disable_outgoing && feature_media_disable_all_video && feature_reload_stream_service; then
        sleep 2
        state_set_publishing false
        state_clear_current_stream_url
        state_recompute_idle
        feature_result_success 0 "idle"
        log_info "stream stop success mode=$mode"
        led_schedule_business_sync
        return 0
    fi

    feature_result_failure -1 "fail"
    log_error "stream stop failed mode=$mode"
    return 1
}

feature_find_latest_record_file() {
    # 停止录像后查找本次录像开始时间之后生成的最新 mp4，作为最后一个分片上传。
    min_ts="$1"
    latest_file=""
    latest_ts=0

    for file_path in $(list_record_files); do
        [ -f "$file_path" ] || continue
        file_ts=$(file_mtime_sec "$file_path")
        [ "$file_ts" -ge "$min_ts" ] || continue
        if [ "$file_ts" -gt "$latest_ts" ]; then
            latest_ts="$file_ts"
            latest_file="$file_path"
        fi
    done

    printf '%s\n' "$latest_file"
}

feature_record_upload_pending_segments() {
    # 常驻 worker 周期扫描稳定的 mp4 文件，上传中间分片并记录 manifest 避免重复上传。
    mode="$1"
    record_id="$2"
    start_ts=$(state_get_record_start_ts)
    now_ts=$(raw_now_sec)
    task_id=$(state_get_current_task_id)

    for file_path in $(list_record_files); do
        [ -f "$file_path" ] || continue
        state_segment_manifest_contains "$file_path" && continue

        file_ts=$(file_mtime_sec "$file_path")
        [ "$file_ts" -ge "$start_ts" ] || continue

        age_sec=$((now_ts - file_ts))
        [ "$age_sec" -ge "$SEGMENT_STABLE_SEC" ] || continue

        next_segment_no=$(( $(state_get_segment_no) + 1 ))
        remote_name=$(build_record_output_name "$next_segment_no" "$task_id")
        named_local_file=$(prepare_record_named_local_file "$file_path" "$remote_name")
        remote_path=$(build_record_remote_path "$record_id" "$remote_name")
        report_url=$(build_ftp_report_url "$remote_path")
        file_size=$(file_size_bytes "$file_path")
        [ -n "$file_size" ] || file_size=0

        log_debug "segment upload attempt mode=$mode record_id=$record_id local_file=$file_path named_file=$named_local_file remote_name=$remote_name remote_path=$remote_path"
        if ftp_upload_file "$named_local_file" "$remote_path"; then
            state_set_segment_no "$next_segment_no"
            state_segment_manifest_add "$file_path"
            protocol_publish_segment_uploaded "$mode" "$record_id" "$remote_name" "$report_url" "$file_size" "$next_segment_no"
            log_info "segment upload success mode=$mode record_id=$record_id segment_no=$next_segment_no file_url=$report_url"
        else
            log_error "segment upload failed mode=$mode record_id=$record_id local_file=$file_path"
        fi
    done
}

feature_record_segment_loop() {
    # 录像期间运行的分片上传循环。停止录像后 state 会变为 false，循环自然退出。
    mode="$1"
    record_id="$2"
    task_id="$3"

    ensure_layout
    state_init
    ensure_runtime_files

    if ! claim_pidfile "$SEGMENT_WORKER_PID_FILE"; then
        log_warn "segment worker already running"
        exit 0
    fi

    trap 'release_pidfile_if_owner "$SEGMENT_WORKER_PID_FILE"' EXIT INT TERM
    log_debug "segment worker start mode=$mode task_id=$task_id record_id=$record_id"

    while [ "$(state_get_recording)" = "true" ] && [ "$(state_get_current_record_id)" = "$record_id" ]; do
        feature_record_upload_pending_segments "$mode" "$record_id"
        sleep "$SEGMENT_SCAN_INTERVAL_SEC"
    done

    log_debug "segment worker exit mode=$mode record_id=$record_id"
}

feature_record_start() {
    # 开始录像：应用视频和录像配置，开启 records，初始化分片状态并启动上传 worker。
    mode="$1"
    requested_record_id="$2"
    task_id="$3"

    ensure_layout
    state_init
    ensure_runtime_files
    feature_result_reset

    RESULT_RECORD_ID="$requested_record_id"
    RESULT_TASK_ID="${task_id:-$(state_get_current_task_id)}"

    if [ -z "$requested_record_id" ]; then
        log_error "record start failed: empty record_id"
        feature_result_failure -1 "fail"
        return 1
    fi

    log_info "record start request mode=$mode record_id=$requested_record_id"

    if feature_media_apply_video_profile && feature_record_apply_profile && feature_record_enable && feature_reload_stream_service; then
        state_set_recording true
        state_set_current_record_id "$requested_record_id"
        state_set_current_record_flow "$mode"
        [ -n "$task_id" ] && state_set_current_task_id "$task_id"
        state_set_record_start_ts "$(raw_now_sec)"
        state_set_record_session_time "$(now_with_format "$RECORD_FILE_TIME_FORMAT")"
        state_reset_segment_no
        state_reset_segment_manifest
        state_recompute_idle
        stop_pidfile_process "$SEGMENT_WORKER_PID_FILE"
        sh "$APP_HOME/app_service.sh" segment_worker "$mode" "$requested_record_id" "$task_id" &
        feature_result_success 0 "recording"
        log_info "record start success mode=$mode record_id=$requested_record_id"
        led_schedule_business_sync
        return 0
    fi

    feature_result_failure -1 "fail"
    log_error "record start failed mode=$mode record_id=$requested_record_id"
    return 1
}

feature_record_stop() {
    # 停止录像：先关闭录像/推流/video，再等待文件落稳，最后上传最终分片并清理状态。
    mode="$1"
    requested_record_id="$2"

    ensure_layout
    state_init
    ensure_runtime_files
    feature_result_reset

    record_id="$requested_record_id"
    [ -n "$record_id" ] || record_id=$(state_get_current_record_id)
    RESULT_RECORD_ID="$record_id"

    if [ -z "$record_id" ]; then
        log_error "record stop failed: empty record_id"
        feature_result_failure -1 "fail"
        return 1
    fi

    log_info "record stop request mode=$mode record_id=$record_id"

    if ! feature_record_disable || ! feature_stream_disable_outgoing || ! feature_media_disable_all_video || ! feature_reload_stream_service; then
        feature_result_failure -1 "fail"
        log_error "record stop failed before final upload record_id=$record_id"
        return 1
    fi

    state_set_recording false
    state_set_publishing false
    state_clear_current_stream_url
    led_schedule_business_sync
    stop_pidfile_process "$SEGMENT_WORKER_PID_FILE"
    sleep "$RECORD_FINALIZE_WAIT_SEC"

    latest_file=$(feature_find_latest_record_file "$(state_get_record_start_ts)")
    if [ -z "$latest_file" ]; then
        feature_result_failure -1 "fail"
        state_clear_current_record_id
        state_clear_current_record_flow
        if [ "$mode" != "task" ]; then
            state_clear_current_task_id
        fi
        state_recompute_idle
        log_error "record stop failed: no record file found record_id=$record_id"
        return 1
    fi

    next_segment_no=$(( $(state_get_segment_no) + 1 ))
    remote_name=$(build_record_output_name "$next_segment_no" "$(state_get_current_task_id)")
    named_local_file=$(prepare_record_named_local_file "$latest_file" "$remote_name")
    remote_path=$(build_record_remote_path "$record_id" "$remote_name")
    report_url=$(build_ftp_report_url "$remote_path")
    file_size=$(file_size_bytes "$latest_file")
    [ -n "$file_size" ] || file_size=0

    log_debug "record stop upload attempt mode=$mode record_id=$record_id local_file=$latest_file named_file=$named_local_file remote_name=$remote_name remote_path=$remote_path"
    if ftp_upload_file "$named_local_file" "$remote_path"; then
        RESULT_FILE_NAME="$remote_name"
        RESULT_FILE_URL="$report_url"
        RESULT_FILE_SIZE="$file_size"
        RESULT_SEGMENT_NO="$next_segment_no"
        state_set_segment_no "$next_segment_no"
        if [ "$mode" = "task" ]; then
            feature_result_success 0 "success"
        else
            feature_result_success 0 "idle"
        fi
        log_info "record stop success mode=$mode record_id=$record_id segment_no=$next_segment_no file_url=$report_url"
    else
        feature_result_failure -1 "fail"
        log_error "record stop upload failed mode=$mode record_id=$record_id local_file=$latest_file"
        return 1
    fi

    state_clear_current_record_id
    state_clear_current_record_flow
    state_set_record_start_ts 0
    state_clear_record_session_time
    state_reset_segment_manifest
    if [ "$mode" != "task" ]; then
        state_clear_current_task_id
    fi
    state_recompute_idle
    return 0
}

feature_capture_take() {
    # 抓拍使用本机 snapshot URL 获取 jpg，再按 capture/<device_id>/<capture_id>.jpg 上传。
    capture_id="$1"

    ensure_layout
    state_init
    ensure_runtime_files
    feature_result_reset

    [ -n "$capture_id" ] || capture_id=$(current_now_ms)
    local_file="${CAPTURE_LOCAL_DIR}/${capture_id}.jpg"
    remote_path=$(build_capture_remote_path "$capture_id")
    report_url=$(build_ftp_report_url "$remote_path")

    RESULT_CAPTURE_ID="$capture_id"
    RESULT_FILE_NAME="${capture_id}.jpg"
    RESULT_FILE_URL="$report_url"

    log_info "capture request capture_id=$capture_id snapshot_url=$CAPTURE_SNAPSHOT_URL"

    if curl -sS -o "$local_file" "$CAPTURE_SNAPSHOT_URL" && [ -s "$local_file" ] && ftp_upload_file "$local_file" "$remote_path"; then
        feature_result_success 0 "success"
        log_info "capture success capture_id=$capture_id local_file=$local_file remote_path=$remote_path"
        return 0
    fi

    rm -f "$local_file"
    feature_result_failure -1 "fail"
    RESULT_FILE_URL=""
    log_error "capture failed capture_id=$capture_id"
    return 1
}

feature_audio_play() {
    # 当前版本按需求禁用音频播放，但仍返回成功 ACK，避免控制端流程被阻塞。
    requested_file_name="$1"
    requested_file_path="$2"

    ensure_layout
    state_init
    ensure_runtime_files
    feature_result_reset

    RESULT_AUDIO_FILE_NAME="$requested_file_name"
    [ -n "$RESULT_AUDIO_FILE_NAME" ] || RESULT_AUDIO_FILE_NAME=$(basename "$requested_file_path")
    [ -n "$RESULT_AUDIO_FILE_NAME" ] || RESULT_AUDIO_FILE_NAME="audio_disabled.wav"

    log_warn "audio playback disabled by current requirement file_name=$requested_file_name file_path=$requested_file_path"
    feature_result_success 0 "success"
    return 0
}

feature_task_prepare_voice() {
    # 旧版实验准备语音命令暂不做实际动作，仅保留协议兼容。
    task_id="$1"
    log_warn "task prepare voice skipped because legacy voice feature is disabled task_id=$task_id"
    return 0
}

feature_task_prepare_desk_voice() {
    # 桌牌识别准备语音交给独立 worker 播放，避免阻塞 MQTT listener。
    task_id="$1"
    log_info "task prepare desk recognition voice request task_id=$task_id"
    sh "$APP_HOME/voice.sh" desk "$task_id" &
    return 0
}

feature_task_hand_voice_start() {
    # 手势识别语音开始命令暂不做实际动作。
    task_id="$1"
    record_id="$2"
    log_warn "task hand voice start skipped because voice feature is disabled task_id=$task_id record_id=$record_id"
    return 0
}

feature_task_hand_voice_stop() {
    # 手势识别语音停止命令需要回 ACK，因此写入 RESULT_* 后返回成功。
    task_id="$1"
    record_id="$2"

    ensure_layout
    state_init
    ensure_runtime_files
    feature_result_reset

    RESULT_TASK_ID="${task_id:-$(state_get_current_task_id)}"
    RESULT_RECORD_ID="${record_id:-$(state_get_current_record_id)}"
    log_warn "task hand voice stop ack as success because voice feature is disabled task_id=$task_id record_id=$record_id"
    feature_result_success 0 "success"
    return 0
}

feature_reset_execute() {
    # 重置命令用于兜底恢复：停止分片 worker、关闭录像/推流/video，并清空所有运行态。
    task_id="$1"

    ensure_layout
    state_init
    ensure_runtime_files
    feature_result_reset

    RESULT_TASK_ID="${task_id:-$(state_get_current_task_id)}"
    reset_code=0

    stop_pidfile_process "$SEGMENT_WORKER_PID_FILE"
    stop_pidfile_process "$VOICE_PLAYER_PID_FILE"

    feature_record_disable || reset_code=-1
    feature_stream_disable_outgoing || reset_code=-1
    feature_media_disable_all_video || reset_code=-1
    feature_reload_stream_service || reset_code=-1

    state_set_recording false
    state_set_publishing false
    state_set_record_start_ts 0
    state_clear_record_session_time
    state_reset_segment_no
    state_reset_segment_manifest
    state_clear_current_record_id
    state_clear_current_record_flow
    state_clear_current_stream_url
    state_clear_current_task_id
    state_recompute_idle

    if [ "$reset_code" = "0" ]; then
        feature_result_success 0 "success"
        log_info "reset success task_id=$task_id"
        sleep "$LED_STATUS_DELAY_SEC"
        led_runtime_reset_idle
        return 0
    fi

    RESULT_CODE="$reset_code"
    feature_result_failure -1 "fail"
    log_error "reset partial failure task_id=$task_id"
    sleep "$LED_STATUS_DELAY_SEC"
    led_runtime_reset_idle
    return 1
}
