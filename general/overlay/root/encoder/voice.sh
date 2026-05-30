#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

# 桌牌识别语音配置。后续调整播放次数或播报间隔时，只需要修改这里。
# desk_8k.pcm 是 8kHz 的原始 PCM 文件，当前语音长度约 5.6 秒。
VOICE_DESK_PCM_FILE="${VOICE_DESK_PCM_FILE:-/mnt/mmcblk0p1/desk_8k.pcm}" # 桌牌识别语音 PCM 文件。
VOICE_PLAY_URL="${VOICE_PLAY_URL:-http://127.0.0.1/play_audio}" # Majestic 本地语音播放接口。
VOICE_REPEAT_COUNT="${VOICE_REPEAT_COUNT:-3}" # 一次指令重复播报次数。
VOICE_AUDIO_DURATION_SEC="${VOICE_AUDIO_DURATION_SEC:-4}" # 单次语音播放预留时长。
VOICE_GAP_SEC="${VOICE_GAP_SEC:-2}" # 两次语音之间的额外空白间隔。
VOICE_CONNECT_TIMEOUT_SEC="${VOICE_CONNECT_TIMEOUT_SEC:-3}" # 连接播放接口的超时时间。
VOICE_REQUEST_TIMEOUT_SEC="${VOICE_REQUEST_TIMEOUT_SEC:-10}" # 提交一次 PCM 的最长等待时间。
VOICE_RELOAD_WAIT_SEC="${VOICE_RELOAD_WAIT_SEC:-1}" # 修改音频输出开关后等待 Majestic 重载的时间。
VOICE_CHILD_PID=""

voice_set_output_enabled() {
    # 播放前临时打开 Majestic 音频输出；修改 YAML 后必须通知 Majestic 重载。
    enabled="$1"
    quoted_config_file=$(shell_quote "$STREAM_CONFIG_FILE")
    run_config_command "voice_output_$enabled" "yaml-cli -i $quoted_config_file -s .audio.outputEnabled $enabled" || return 1
    stream_service_reload_or_recover "voice_output_reload_$enabled" || return 1
    sleep "$VOICE_RELOAD_WAIT_SEC"
}

voice_cleanup() {
    # 正常结束、播放失败或收到中断信号时释放 pidfile。音频输出保持开启，供后续播报复用。
    voice_stop_child
    release_pidfile_if_owner "$VOICE_PLAYER_PID_FILE"
}

voice_stop_child() {
    # 语音 worker 退出时停止当前 curl/sleep，避免后台残留子进程。
    [ -n "$VOICE_CHILD_PID" ] || return 0

    if kill -0 "$VOICE_CHILD_PID" 2>/dev/null; then
        kill "$VOICE_CHILD_PID" 2>/dev/null
        wait "$VOICE_CHILD_PID" 2>/dev/null
    fi
    VOICE_CHILD_PID=""
}

voice_handle_signal() {
    # shell 等待外部命令时可能延迟退出，先主动终止当前子进程。
    exit_code="$1"
    voice_stop_child
    exit "$exit_code"
}

voice_wait() {
    # 后台等待使 TERM/INT 能够立即触发 trap。
    sleep "$1" &
    VOICE_CHILD_PID=$!
    wait "$VOICE_CHILD_PID"
    rc=$?
    VOICE_CHILD_PID=""
    return "$rc"
}

voice_submit_pcm() {
    # Majestic 接口接收原始 PCM 数据，提交完成后由 voice_wait 预留播放时间。
    err_file="${TMP_DIR}/voice_curl_$$.err"
    rm -f "$err_file"
    curl -fsS \
        --connect-timeout "$VOICE_CONNECT_TIMEOUT_SEC" \
        --max-time "$VOICE_REQUEST_TIMEOUT_SEC" \
        --data-binary "@$VOICE_DESK_PCM_FILE" \
        "$VOICE_PLAY_URL" >/dev/null 2>"$err_file" &
    VOICE_CHILD_PID=$!
    wait "$VOICE_CHILD_PID"
    rc=$?
    VOICE_CHILD_PID=""

    if [ "$rc" != "0" ]; then
        curl_error=$(tr '\r\n' '  ' < "$err_file" 2>/dev/null)
        [ -n "$curl_error" ] || curl_error="empty"
        log_error_tag "VOICE" "PCM submit failed rc=$rc url=$VOICE_PLAY_URL curl_error=$curl_error"
    fi
    rm -f "$err_file"
    return "$rc"
}

voice_play_desk() {
    # 桌牌识别语音在独立 worker 中播放，避免阻塞 MQTT listener。
    task_id="$1"

    ensure_layout
    require_command yaml-cli || return 1
    require_command curl || return 1

    if [ ! -f "$VOICE_DESK_PCM_FILE" ]; then
        log_error_tag "VOICE" "desk pcm file missing: $VOICE_DESK_PCM_FILE"
        return 1
    fi

    if ! claim_pidfile "$VOICE_PLAYER_PID_FILE"; then
        log_warn_tag "VOICE" "desk voice already playing, ignore duplicate task_id=$task_id"
        return 0
    fi

    trap 'voice_cleanup' EXIT
    trap 'voice_handle_signal 130' INT
    trap 'voice_handle_signal 143' TERM

    voice_set_output_enabled true || return 1
    log_info_tag "VOICE" "desk voice start task_id=$task_id repeat=$VOICE_REPEAT_COUNT file=$VOICE_DESK_PCM_FILE"

    current=1
    while [ "$current" -le "$VOICE_REPEAT_COUNT" ]; do
        if ! voice_submit_pcm; then
            log_error_tag "VOICE" "desk voice play failed task_id=$task_id repeat=$current"
            return 1
        fi

        log_debug_tag "VOICE" "desk voice submitted task_id=$task_id repeat=$current"
        # 先等待当前 PCM 播放完毕，再追加可配置的空白间隔。
        voice_wait "$VOICE_AUDIO_DURATION_SEC" || return 1
        if [ "$current" -lt "$VOICE_REPEAT_COUNT" ]; then
            voice_wait "$VOICE_GAP_SEC" || return 1
        fi
        current=$((current + 1))
    done

    log_info_tag "VOICE" "desk voice finished task_id=$task_id repeat=$VOICE_REPEAT_COUNT"
    return 0
}

case "${1:-}" in
    desk)
        shift
        voice_play_desk "$@"
        ;;
    *)
        echo "usage: sh voice.sh desk [task_id]" >&2
        exit 1
        ;;
esac
