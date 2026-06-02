#!/bin/sh

# 状态文件层。
# 运行态以一个字段一个文件的方式保存，方便 BusyBox shell 读写，也方便现场直接查看。
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

state_read() {
    # 读取状态文件，不存在时返回默认值。
    file="$1"
    default_value="$2"
    if [ -f "$file" ]; then
        cat "$file" 2>/dev/null
    else
        printf '%s\n' "$default_value"
    fi
}

state_write() {
    # 写入状态文件。调用方负责传入已经校验过的值。
    file="$1"
    value="$2"
    printf '%s\n' "$value" > "$file"
}

state_clear() {
    # 清空字符串型状态，例如当前 task_id/record_id/stream_url。
    : > "$1"
}

state_init() {
    # 初始化所有状态文件。已经存在的值不会覆盖，避免进程重启丢失当前业务状态。
    ensure_layout
    [ -f "$STATE_IDLE_FILE" ] || echo true > "$STATE_IDLE_FILE"
    [ -f "$STATE_RECORDING_FILE" ] || echo false > "$STATE_RECORDING_FILE"
    [ -f "$STATE_PUBLISHING_FILE" ] || echo false > "$STATE_PUBLISHING_FILE"
    [ -f "$STATE_CHARGING_FILE" ] || echo false > "$STATE_CHARGING_FILE"
    [ -f "$STATE_BATTERY_FILE" ] || echo 100 > "$STATE_BATTERY_FILE"
    [ -f "$STATE_SIGNAL_FILE" ] || echo 0 > "$STATE_SIGNAL_FILE"
    [ -f "$STATE_CURRENT_TASK_ID_FILE" ] || : > "$STATE_CURRENT_TASK_ID_FILE"
    [ -f "$STATE_CURRENT_RECORD_ID_FILE" ] || : > "$STATE_CURRENT_RECORD_ID_FILE"
    [ -f "$STATE_CURRENT_RECORD_FLOW_FILE" ] || : > "$STATE_CURRENT_RECORD_FLOW_FILE"
    [ -f "$STATE_CURRENT_STREAM_URL_FILE" ] || : > "$STATE_CURRENT_STREAM_URL_FILE"
    [ -f "$STATE_RECORD_START_TS_FILE" ] || echo 0 > "$STATE_RECORD_START_TS_FILE"
    [ -f "$STATE_RECORD_SESSION_TIME_FILE" ] || : > "$STATE_RECORD_SESSION_TIME_FILE"
    [ -f "$STATE_SEGMENT_NO_FILE" ] || echo 0 > "$STATE_SEGMENT_NO_FILE"
    [ -f "$STATE_SEGMENT_MANIFEST_FILE" ] || : > "$STATE_SEGMENT_MANIFEST_FILE"
}

# 以下 getter/setter 是 shell 版本的状态访问接口，避免业务层直接操作具体文件名。
state_get_idle() { state_read "$STATE_IDLE_FILE" true; }
state_set_idle() { state_write "$STATE_IDLE_FILE" "$1"; }
state_get_recording() { state_read "$STATE_RECORDING_FILE" false; }
state_set_recording() { state_write "$STATE_RECORDING_FILE" "$1"; }
state_get_publishing() { state_read "$STATE_PUBLISHING_FILE" false; }
state_set_publishing() { state_write "$STATE_PUBLISHING_FILE" "$1"; }
state_get_charging() { state_read "$STATE_CHARGING_FILE" false; }
state_set_charging() { state_write "$STATE_CHARGING_FILE" "$1"; }
state_get_battery() { state_read "$STATE_BATTERY_FILE" 100; }
state_set_battery() { state_write "$STATE_BATTERY_FILE" "$1"; }
state_get_signal() { state_read "$STATE_SIGNAL_FILE" 0; }
state_set_signal() { state_write "$STATE_SIGNAL_FILE" "$1"; }
state_get_current_task_id() { state_read "$STATE_CURRENT_TASK_ID_FILE" ""; }
state_set_current_task_id() { state_write "$STATE_CURRENT_TASK_ID_FILE" "$1"; }
state_clear_current_task_id() { state_clear "$STATE_CURRENT_TASK_ID_FILE"; }
state_get_current_record_id() { state_read "$STATE_CURRENT_RECORD_ID_FILE" ""; }
state_set_current_record_id() { state_write "$STATE_CURRENT_RECORD_ID_FILE" "$1"; }
state_clear_current_record_id() { state_clear "$STATE_CURRENT_RECORD_ID_FILE"; }
state_get_current_record_flow() { state_read "$STATE_CURRENT_RECORD_FLOW_FILE" ""; }
state_set_current_record_flow() { state_write "$STATE_CURRENT_RECORD_FLOW_FILE" "$1"; }
state_clear_current_record_flow() { state_clear "$STATE_CURRENT_RECORD_FLOW_FILE"; }
state_get_current_stream_url() { state_read "$STATE_CURRENT_STREAM_URL_FILE" ""; }
state_set_current_stream_url() { state_write "$STATE_CURRENT_STREAM_URL_FILE" "$1"; }
state_clear_current_stream_url() { state_clear "$STATE_CURRENT_STREAM_URL_FILE"; }
state_get_record_start_ts() { state_read "$STATE_RECORD_START_TS_FILE" 0; }
state_set_record_start_ts() { state_write "$STATE_RECORD_START_TS_FILE" "$1"; }
state_get_record_session_time() { state_read "$STATE_RECORD_SESSION_TIME_FILE" ""; }
state_set_record_session_time() { state_write "$STATE_RECORD_SESSION_TIME_FILE" "$1"; }
state_clear_record_session_time() { state_clear "$STATE_RECORD_SESSION_TIME_FILE"; }
state_get_segment_no() { state_read "$STATE_SEGMENT_NO_FILE" 0; }
state_set_segment_no() { state_write "$STATE_SEGMENT_NO_FILE" "$1"; }
state_reset_segment_no() { state_write "$STATE_SEGMENT_NO_FILE" 0; }

state_reset_segment_manifest() {
    # 分片清单记录已经上传过的本地 mp4，重置后新的录像会重新开始跟踪。
    : > "$STATE_SEGMENT_MANIFEST_FILE"
}

state_segment_manifest_contains() {
    # 判断某个本地 mp4 是否已经上传过，防止分片 worker 重复上报。
    file_path="$1"
    grep -Fx "$file_path" "$STATE_SEGMENT_MANIFEST_FILE" >/dev/null 2>&1
}

state_segment_manifest_add() {
    # 上传成功后把本地文件路径追加到 manifest。
    printf '%s\n' "$1" >> "$STATE_SEGMENT_MANIFEST_FILE"
}

state_recompute_idle() {
    # 空闲状态是派生状态：只要正在录像或推流，就不是空闲。
    if [ "$(state_get_recording)" = "true" ] || [ "$(state_get_publishing)" = "true" ]; then
        state_set_idle false
    else
        state_set_idle true
    fi
}

state_dump() {
    # 配置页面使用的状态快照，便于远程排查当前设备处于什么流程。
    printf 'is_idle=%s\n' "$(state_get_idle)"
    printf 'is_recording=%s\n' "$(state_get_recording)"
    printf 'is_publishing=%s\n' "$(state_get_publishing)"
    printf 'is_charging=%s\n' "$(state_get_charging)"
    printf 'battery=%s\n' "$(state_get_battery)"
    printf 'signal=%s\n' "$(state_get_signal)"
    printf 'task_id=%s\n' "$(state_get_current_task_id)"
    printf 'record_id=%s\n' "$(state_get_current_record_id)"
    printf 'record_flow=%s\n' "$(state_get_current_record_flow)"
    printf 'stream_url=%s\n' "$(state_get_current_stream_url)"
    printf 'record_start_ts=%s\n' "$(state_get_record_start_ts)"
    printf 'record_session_time=%s\n' "$(state_get_record_session_time)"
    printf 'segment_no=%s\n' "$(state_get_segment_no)"
}
