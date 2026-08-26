#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

# 桌牌识别语音配置。后续调整播放次数或播报间隔时，只需要修改这里。
# desk_8k.pcm 是 8kHz、16-bit、单声道原始 PCM，当前语音长度约 2.8 秒。
RESOURCE_DIR="${RESOURCE_DIR:-/root/resources}" # 固件内置资源目录。
VOICE_DESK_PCM_FILE="${VOICE_DESK_PCM_FILE:-${RESOURCE_DIR}/desk_8k.pcm}" # 桌牌识别语音 PCM 文件。
VOICE_PLAY_URL="${VOICE_PLAY_URL:-http://127.0.0.1/play_audio}" # Majestic 本地语音播放接口。
VOICE_SAMPLE_RATE="${VOICE_SAMPLE_RATE:-8000}" # 原始 PCM 采样率，必须与播报文件一致。
VOICE_CODEC="${VOICE_CODEC:-opus}" # Majestic 主音频编码器；启用 audio 后才能创建 ADEC/AO 播放通道。
VOICE_INPUT_VOLUME="${VOICE_INPUT_VOLUME:-30}" # Majestic 音频输入音量，使用固件安全默认值。
VOICE_OUTPUT_VOLUME="${VOICE_OUTPUT_VOLUME:-80}" # 扬声器输出音量。
VOICE_REPEAT_COUNT="${VOICE_REPEAT_COUNT:-3}" # 一次指令重复播报次数。
VOICE_AUDIO_DURATION_SEC="${VOICE_AUDIO_DURATION_SEC:-4}" # 单次语音播放预留时长，应覆盖约 2.8 秒的完整 PCM。
VOICE_GAP_SEC="${VOICE_GAP_SEC:-2}" # 两次语音之间的额外空白间隔。
VOICE_CONNECT_TIMEOUT_SEC="${VOICE_CONNECT_TIMEOUT_SEC:-3}" # 连接播放接口的超时时间。
VOICE_REQUEST_TIMEOUT_SEC="${VOICE_REQUEST_TIMEOUT_SEC:-10}" # 提交一次 PCM 的最长等待时间。
VOICE_RELOAD_WAIT_SEC="${VOICE_RELOAD_WAIT_SEC:-2}" # 每次通知 Majestic 重载后等待服务恢复的时间。
VOICE_RELOAD_RETRY_SEC="${VOICE_RELOAD_RETRY_SEC:-3}" # HUP 被 Majestic 防抖忽略时，重试前额外等待时间。
VOICE_RELOAD_RETRY_COUNT="${VOICE_RELOAD_RETRY_COUNT:-3}" # 等待音频输出接口就绪的最大重载次数。
VOICE_PREEMPT_WAIT_SEC="${VOICE_PREEMPT_WAIT_SEC:-1}" # 抢占旧播报时等待其响应 TERM 的时间。
VOICE_LOCK_RETRY_COUNT="${VOICE_LOCK_RETRY_COUNT:-5}" # 并发抢占语音锁的最大重试次数。
VOICE_PLAYER_LOCK_DIR="${VOICE_PLAYER_PID_FILE}.lock"
VOICE_CHILD_PID=""

voice_set_output_enabled() {
    # outputEnabled 只开放 HTTP 播放接口；audio.enabled 负责创建底层 ADEC/AO 通道。
    # 两者必须同时开启，否则 /play_audio 可能返回 200，但日志会报 ERR_ADEC_UNEXIST 且扬声器无声。
    enabled="$1"
    quoted_config_file=$(shell_quote "$STREAM_CONFIG_FILE")
    encoder_exit_if_main_stopped "voice"

    if [ "$enabled" = "true" ]; then
        quoted_codec=$(shell_quote "$VOICE_CODEC")
        run_config_command "voice_audio_enabled" "yaml-cli -i $quoted_config_file -s .audio.enabled true" || return 1
        run_config_command "voice_audio_volume" "yaml-cli -i $quoted_config_file -s .audio.volume $VOICE_INPUT_VOLUME" || return 1
        run_config_command "voice_audio_srate" "yaml-cli -i $quoted_config_file -s .audio.srate $VOICE_SAMPLE_RATE" || return 1
        run_config_command "voice_audio_codec" "yaml-cli -i $quoted_config_file -s .audio.codec $quoted_codec" || return 1
        run_config_command "voice_output_volume" "yaml-cli -i $quoted_config_file -s .audio.outputVolume $VOICE_OUTPUT_VOLUME" || return 1
    fi

    run_config_command "voice_output_$enabled" "yaml-cli -i $quoted_config_file -s .audio.outputEnabled $enabled" || return 1

    reload_attempt=1
    while [ "$reload_attempt" -le "$VOICE_RELOAD_RETRY_COUNT" ]; do
        encoder_exit_if_main_stopped "voice"
        stream_service_reload_or_recover "voice_output_reload_${enabled}_$reload_attempt" || return 1
        sleep "$VOICE_RELOAD_WAIT_SEC"

        if [ "$enabled" != "true" ] || voice_output_is_ready; then
            return 0
        fi

        log_warn_tag "VOICE" "audio output not ready after reload attempt=$reload_attempt"
        reload_attempt=$((reload_attempt + 1))
        [ "$reload_attempt" -le "$VOICE_RELOAD_RETRY_COUNT" ] && sleep "$VOICE_RELOAD_RETRY_SEC"
    done

    log_error_tag "VOICE" "audio output did not become ready retries=$VOICE_RELOAD_RETRY_COUNT url=$VOICE_PLAY_URL"
    return 1
}

voice_output_is_ready() {
    ready_http_code=$(curl -sS \
        --connect-timeout "$VOICE_CONNECT_TIMEOUT_SEC" \
        --max-time "$VOICE_REQUEST_TIMEOUT_SEC" \
        -o /dev/null \
        -w '%{http_code}' \
        "$VOICE_PLAY_URL" 2>/dev/null) || return 1
    [ "$ready_http_code" = "200" ]
}

voice_pid_is_running() {
    pid="$1"
    case "$pid" in
        ""|*[!0-9]*)
            return 1
            ;;
    esac

    kill -0 "$pid" 2>/dev/null
}

voice_pid_is_worker() {
    pid="$1"
    voice_pid_is_running "$pid" || return 1

    # 防止旧锁中的 PID 被系统复用后误杀无关进程。
    if [ -r "/proc/$pid/cmdline" ]; then
        tr '\000' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -F "voice.sh" >/dev/null 2>&1
        return $?
    fi

    return 0
}

voice_lock_owner_pid() {
    cat "$VOICE_PLAYER_LOCK_DIR/pid" 2>/dev/null
}

voice_lock_owner_task_id() {
    cat "$VOICE_PLAYER_LOCK_DIR/task_id" 2>/dev/null
}

voice_remove_lock_for_pid() {
    expected_pid="$1"
    [ -d "$VOICE_PLAYER_LOCK_DIR" ] || return 0

    lock_pid=$(voice_lock_owner_pid)
    if [ -n "$expected_pid" ]; then
        [ "$lock_pid" = "$expected_pid" ] || return 1
    else
        [ -z "$lock_pid" ] || return 1
    fi

    rm -f "$VOICE_PLAYER_LOCK_DIR/pid" "$VOICE_PLAYER_LOCK_DIR/task_id"
    rmdir "$VOICE_PLAYER_LOCK_DIR" 2>/dev/null
}

voice_release_task_lock() {
    voice_remove_lock_for_pid "$$"
    release_pidfile_if_owner "$VOICE_PLAYER_PID_FILE"
}

voice_terminate_owner() {
    owner_pid="$1"
    voice_pid_is_worker "$owner_pid" || return 0

    kill "$owner_pid" 2>/dev/null
    sleep "$VOICE_PREEMPT_WAIT_SEC"
    if voice_pid_is_running "$owner_pid"; then
        kill -9 "$owner_pid" 2>/dev/null
    fi

    if voice_pid_is_worker "$owner_pid"; then
        log_error_tag "VOICE" "cannot stop old desk voice worker pid=$owner_pid"
        return 1
    fi

    return 0
}

voice_claim_task_lock() {
    requested_task_id="$1"
    attempt=1

    while [ "$attempt" -le "$VOICE_LOCK_RETRY_COUNT" ]; do
        encoder_exit_if_main_stopped "voice"
        if mkdir "$VOICE_PLAYER_LOCK_DIR" 2>/dev/null; then
            legacy_pid=$(cat "$VOICE_PLAYER_PID_FILE" 2>/dev/null)

            printf '%s\n' "$$" > "$VOICE_PLAYER_LOCK_DIR/pid"
            printf '%s\n' "$requested_task_id" > "$VOICE_PLAYER_LOCK_DIR/task_id"
            printf '%s\n' "$$" > "$VOICE_PLAYER_PID_FILE"

            if [ -n "$legacy_pid" ] && [ "$legacy_pid" != "$$" ] && voice_pid_is_worker "$legacy_pid"; then
                log_warn_tag "VOICE" "preempt legacy desk voice worker old_pid=$legacy_pid new_task_id=$requested_task_id"
                if ! voice_terminate_owner "$legacy_pid"; then
                    voice_release_task_lock
                    return 1
                fi
            fi
            return 0
        fi

        owner_pid=$(voice_lock_owner_pid)
        owner_task_id=$(voice_lock_owner_task_id)

        # mkdir 成功到元数据落盘之间存在极短窗口，先等待一次再判断为残留锁。
        if [ -z "$owner_pid" ]; then
            sleep 1
            owner_pid=$(voice_lock_owner_pid)
            owner_task_id=$(voice_lock_owner_task_id)
        fi

        if [ -n "$owner_pid" ] && voice_pid_is_worker "$owner_pid"; then
            if [ "$owner_task_id" = "$requested_task_id" ]; then
                log_warn_tag "VOICE" "same desk voice task already playing, ignore duplicate task_id=$requested_task_id owner_pid=$owner_pid"
                return 2
            fi

            log_warn_tag "VOICE" "desk voice busy, preempt old task old_task_id=${owner_task_id:-unknown} old_pid=$owner_pid new_task_id=$requested_task_id"
            voice_terminate_owner "$owner_pid" || return 1
        else
            log_warn_tag "VOICE" "remove stale desk voice lock owner_pid=${owner_pid:-unknown} new_task_id=$requested_task_id"
        fi

        voice_remove_lock_for_pid "$owner_pid"
        attempt=$((attempt + 1))
    done

    log_error_tag "VOICE" "cannot acquire desk voice lock task_id=$requested_task_id retries=$VOICE_LOCK_RETRY_COUNT"
    return 1
}

voice_cleanup() {
    # 正常结束、播放失败或收到中断信号时释放任务锁。音频输出保持开启，供后续播报复用。
    voice_stop_child
    voice_release_task_lock
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

voice_wait_with_main_guard() {
    total_sec="$1"
    check_sec="${CHILD_EXIT_CHECK_SEC:-1}"

    case "$check_sec" in
        ''|*[!0-9]*|0)
            check_sec=1
            ;;
    esac

    waited_sec=0
    while [ "$waited_sec" -lt "$total_sec" ]; do
        encoder_exit_if_main_stopped "voice"
        remaining_sec=$((total_sec - waited_sec))
        sleep_sec="$check_sec"
        [ "$sleep_sec" -le "$remaining_sec" ] || sleep_sec="$remaining_sec"
        [ "$sleep_sec" -gt 0 ] || break
        voice_wait "$sleep_sec" || return 1
        waited_sec=$((waited_sec + sleep_sec))
    done
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
    encoder_exit_if_main_stopped "voice"

    if [ -z "$task_id" ]; then
        log_error_tag "VOICE" "desk voice task_id missing"
        return 1
    fi

    require_command yaml-cli || return 1
    require_command curl || return 1

    if [ ! -f "$VOICE_DESK_PCM_FILE" ]; then
        log_error_tag "VOICE" "desk pcm file missing: $VOICE_DESK_PCM_FILE"
        return 1
    fi

    voice_claim_task_lock "$task_id"
    claim_rc=$?
    case "$claim_rc" in
        0)
            ;;
        2)
            return 0
            ;;
        *)
            return 1
            ;;
    esac

    trap 'voice_cleanup' EXIT
    trap 'voice_handle_signal 130' INT
    trap 'voice_handle_signal 143' TERM

    encoder_exit_if_main_stopped "voice"
    voice_set_output_enabled true || return 1
    log_info_tag "VOICE" "desk voice start task_id=$task_id repeat=$VOICE_REPEAT_COUNT file=$VOICE_DESK_PCM_FILE"

    current=1
    while [ "$current" -le "$VOICE_REPEAT_COUNT" ]; do
        encoder_exit_if_main_stopped "voice"
        if ! voice_submit_pcm; then
            log_error_tag "VOICE" "desk voice play failed task_id=$task_id repeat=$current"
            return 1
        fi

        log_debug_tag "VOICE" "desk voice submitted task_id=$task_id repeat=$current"
        # 先等待当前 PCM 播放完毕，再追加可配置的空白间隔。
        voice_wait_with_main_guard "$VOICE_AUDIO_DURATION_SEC" || return 1
        if [ "$current" -lt "$VOICE_REPEAT_COUNT" ]; then
            voice_wait_with_main_guard "$VOICE_GAP_SEC" || return 1
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
