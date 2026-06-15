#!/bin/sh

# 通用工具层。
# 提供目录初始化、日志、命令检查、pidfile、文件信息和安全清理等基础能力。
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/config.sh"

ensure_layout() {
    # 所有脚本启动前都可以重复调用，确保运行目录存在并初始化 msgId 文件。
    mkdir -p "$WORKDIR" "$STATE_DIR" "$LOG_DIR" "$TMP_DIR" "$RECORD_NAMED_LOCAL_DIR" "$CAPTURE_LOCAL_DIR" "$AUDIO_LOCAL_DIR"
    [ -f "$MSGID_FILE" ] || echo 1000 > "$MSGID_FILE"
}

now_str() {
    # 日志时间使用板端本地时间，便于和现场终端输出对齐。
    date '+%F %T'
}

raw_now_sec() {
    # 原始系统秒级时间，不叠加云端时间偏移。
    date '+%s'
}

raw_now_ms() {
    # 在 shell 里统一使用毫秒时间戳，便于和协议中的 timestamp 对齐。
    echo $(( $(raw_now_sec) * 1000 ))
}

script_name() {
    # 日志中记录当前脚本名，方便区分主进程、服务进程和 worker。
    basename "${0:-sh}"
}

print_title() {
    # 终端页面标题，安装、配置、删除和主程序都会使用。
    ensure_layout
    printf '===== %s %s =====\n' "$1" "$DEVICE_VERSION"
}

log_write() {
    # 统一日志格式。ENCODER_STDOUT_LOG=0 时只写文件，不打到终端。
    level="$1"
    shift

    ensure_layout
    line="$(now_str) [$DEVICE_VERSION] [$(script_name)] [$level] $*"
    printf '%s\n' "$line" >> "$LOGFILE"

    stdout_enabled="${ENCODER_STDOUT_LOG:-1}"
    [ "$stdout_enabled" = "1" ] || return 0

    case "$level" in
        ERROR)
            printf '%s\n' "$line" >&2
            ;;
        *)
            printf '%s\n' "$line"
            ;;
    esac
}

log_info() {
    # 普通信息日志。
    log_write "INFO" "$@"
}

log_info_tag() {
    # 带模块标签的信息日志，便于 grep 过滤。
    tag="$1"
    shift
    log_write "INFO" "[$tag] $*"
}

log_debug() {
    # 调试日志默认关闭，避免现场运行时输出过多。
    [ "$LOG_VERBOSE" = "true" ] || return 0
    log_write "DEBUG" "$@"
}

log_debug_tag() {
    # 带模块标签的 DEBUG 日志。
    [ "$LOG_VERBOSE" = "true" ] || return 0
    tag="$1"
    shift
    log_write "DEBUG" "[$tag] $*"
}

log_warn() {
    # 警告日志：动作可继续，但需要关注。
    log_write "WARN" "$@"
}

log_warn_tag() {
    # 带模块标签的警告日志。
    tag="$1"
    shift
    log_write "WARN" "[$tag] $*"
}

log_error() {
    # 错误日志会写 stderr，便于 shell 调用方捕获。
    log_write "ERROR" "$@"
}

log_error_tag() {
    # 带模块标签的错误日志。
    tag="$1"
    shift
    log_write "ERROR" "[$tag] $*"
}

command_exists() {
    # 判断命令是否存在，兼容 BusyBox/OpenIPC 环境。
    command -v "$1" >/dev/null 2>&1
}

shell_quote() {
    # 为拼接 cli 命令准备单引号转义后的字符串。
    value="$1"
    escaped=$(printf '%s' "$value" | sed "s/'/'\\\\''/g")
    printf "'%s'\n" "$escaped"
}

require_command() {
    # 外部依赖缺失时统一记录错误。
    if command_exists "$1"; then
        return 0
    fi

    log_error "required command missing: $1"
    return 1
}

is_valid_device_id() {
    # 设备 ID 会进入 topic、文件名和路径，只允许安全字符。
    value="$1"
    [ -n "$value" ] || return 1

    sanitized=$(printf '%s' "$value" | sed 's/[^A-Za-z0-9._-]/_/g')
    [ "$sanitized" = "$value" ]
}

ensure_device_id_configured() {
    # 启动前强校验 device_id，避免设备注册到错误 topic 或生成非法文件名。
    if [ "$DEVICE_ID_CONFIGURED" != "true" ] || [ -z "$DEVICE_ID" ]; then
        log_error "device id missing: export DEVICE_ID before starting encoder"
        return 1
    fi

    if ! is_valid_device_id "$DEVICE_ID"; then
        log_error "device id invalid: DEVICE_ID=$DEVICE_ID allowed=A-Za-z0-9._-"
        return 1
    fi

    return 0
}

next_msgid() {
    # 消息 ID（MQTT msgId）简单递增并落盘，重启后继续从上次值往后走。
    ensure_layout
    n=$(cat "$MSGID_FILE" 2>/dev/null)
    [ -n "$n" ] || n=1000
    n=$((n + 1))
    printf '%s\n' "$n" > "$MSGID_FILE"
    printf '%s\n' "$n"
}

get_ip_addr() {
    # 预留的本机 IP 获取逻辑：优先 eth0，其次 wlan0，最后 hostname -I。
    ip=$(ip addr show eth0 2>/dev/null | awk '/inet / {print $2}' | cut -d/ -f1 | head -n 1)
    [ -n "$ip" ] && {
        printf '%s\n' "$ip"
        return
    }

    ip=$(ip addr show wlan0 2>/dev/null | awk '/inet / {print $2}' | cut -d/ -f1 | head -n 1)
    [ -n "$ip" ] && {
        printf '%s\n' "$ip"
        return
    }

    ip=$(hostname -I 2>/dev/null | awk '{print $1}')
    [ -n "$ip" ] && {
        printf '%s\n' "$ip"
        return
    }

    printf '0.0.0.0\n'
}

pid_matches_command() {
    # 除了检查 PID 存活，还可校验命令行，避免重启后旧 PID 被其它进程复用。
    pid="$1"
    expected_command="$2"

    case "$pid" in
        ''|*[!0-9]*)
            return 1
            ;;
    esac

    kill -0 "$pid" 2>/dev/null || return 1
    [ -n "$expected_command" ] || return 0
    [ -r "/proc/$pid/cmdline" ] || return 1

    tr '\000' ' ' < "/proc/$pid/cmdline" | grep -F "$expected_command" >/dev/null 2>&1
}

is_pid_running_file() {
    # 根据 pidfile 判断进程是否仍然存活，第二个参数可指定预期命令行。
    pidfile="$1"
    expected_command="$2"
    [ -f "$pidfile" ] || return 1

    pid=$(cat "$pidfile" 2>/dev/null)

    pid_matches_command "$pid" "$expected_command"
}

claim_pidfile() {
    # 抢占 pidfile。已有活进程时返回失败，避免同类服务重复启动。
    pidfile="$1"
    expected_command="$2"
    current_pid="$$"

    if [ -f "$pidfile" ]; then
        existing_pid=$(cat "$pidfile" 2>/dev/null)
        if [ "$existing_pid" != "$current_pid" ] && pid_matches_command "$existing_pid" "$expected_command"; then
            return 1
        fi
    fi

    printf '%s\n' "$current_pid" > "$pidfile"
    return 0
}

release_pidfile_if_owner() {
    # 只有 pidfile 中记录的是当前进程时才删除，避免误删别人的锁。
    pidfile="$1"
    current_pid="$$"

    [ -f "$pidfile" ] || return 0
    existing_pid=$(cat "$pidfile" 2>/dev/null)
    [ "$existing_pid" = "$current_pid" ] || return 0

    rm -f "$pidfile"
}

stop_pidfile_process() {
    # 按 pidfile 停止进程，先 TERM，仍存活再 KILL。
    pidfile="$1"
    [ -f "$pidfile" ] || return 0

    pid=$(cat "$pidfile" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null
        sleep 1
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null
        fi
    fi

    rm -f "$pidfile"
}

run_config_command() {
    # 执行会修改板端配置的命令。默认只在失败时记录命令细节，减少正常日志噪声。
    cmd_name="$1"
    cmd_text="$2"

    if [ -z "$cmd_text" ]; then
        log_error "command text empty: $cmd_name"
        return 1
    fi

    if [ "$CONFIG_COMMAND_VERBOSE" = "true" ]; then
        log_info "run command [$cmd_name]: $cmd_text"
    fi

    err_file="${TMP_DIR}/cmd_${cmd_name}_$$.err"
    rm -f "$err_file"
    sh -c "$cmd_text" 2>"$err_file"
    rc=$?
    cmd_error=$(tr '\r\n' '  ' < "$err_file" 2>/dev/null)
    rm -f "$err_file"

    if [ "$rc" = "0" ]; then
        if [ "$CONFIG_COMMAND_VERBOSE" = "true" ]; then
            log_info "command [$cmd_name] exit=$rc"
        fi
        return 0
    fi

    [ -n "$cmd_error" ] || cmd_error="empty"
    log_error "command [$cmd_name] failed exit=$rc error=$cmd_error cmd=$cmd_text"
    return "$rc"
}

stream_service_is_running() {
    # Majestic 崩溃时系统 PID 文件可能残留，优先检查真实进程而不是只看 PID 文件。
    if command_exists pidof; then
        pidof "$STREAM_SERVICE_PROCESS" >/dev/null 2>&1
        return $?
    fi

    ps w 2>/dev/null | awk -v name="$STREAM_SERVICE_PROCESS" '
        NR > 1 {
            for (i = 4; i <= NF; i++) {
                if ($i == name) {
                    found = 1
                }
            }
        }
        END { exit(found ? 0 : 1) }
    '
}

stream_service_start_if_missing() {
    # HUP 后如果 Majestic 异常退出，清理旧 PID 文件并通过系统 init 脚本恢复服务。
    stream_service_is_running && return 0

    log_warn_tag "MAJESTIC" "process missing, attempt recovery"
    rm -f "$STREAM_SERVICE_PID_FILE"
    run_config_command "stream_service_start" "$STREAM_SERVICE_START_CMD" || return 1
    sleep "$STREAM_START_WAIT_SEC"

    if stream_service_is_running; then
        log_info_tag "MAJESTIC" "process recovery success"
        return 0
    fi

    log_error_tag "MAJESTIC" "process recovery failed"
    return 1
}

stream_service_reload_or_recover() {
    # 正常情况发送 HUP；如果进程已经退出或重载后退出，则自动走恢复启动。
    reload_label="${1:-stream_reload}"
    reload_rc=0

    if stream_service_is_running; then
        run_config_command "$reload_label" "$STREAM_RELOAD_CMD" || reload_rc=$?
        sleep "$STREAM_RELOAD_WAIT_SEC"
    else
        reload_rc=1
    fi

    if ! stream_service_is_running; then
        stream_service_start_if_missing
        return $?
    fi

    return "$reload_rc"
}

file_mtime_sec() {
    # 获取文件修改时间，找录像分片时用于判断文件是否属于当前会话。
    stat -c %Y "$1" 2>/dev/null || echo 0
}

file_size_bytes() {
    # 获取文件大小，上传成功后上报给控制端。
    wc -c < "$1" 2>/dev/null | tr -d '[:space:]'
}

now_with_format() {
    # 按配置格式生成录像文件名里的时间片段。
    date "+$1"
}

sanitize_name_part() {
    # 清理文件名片段，避免 task_id/device_id 中出现路径或 shell 特殊字符。
    value="$1"
    sanitized=$(printf '%s' "$value" | sed 's/[^A-Za-z0-9._-]/_/g')
    [ -n "$sanitized" ] || sanitized="unknown"
    printf '%s\n' "$sanitized"
}

list_record_files() {
    # 列出 Majestic 本地录像文件，供分片 worker 和停止录像最终上传使用。
    [ -d "$RECORD_SEARCH_ROOT" ] || return 0
    find "$RECORD_SEARCH_ROOT" -type f -name '*.mp4' 2>/dev/null | sort
}

safe_remove_path() {
    # 删除路径前先确认存在，delete_all.sh 使用。
    target="$1"
    [ -e "$target" ] || return 0
    rm -rf "$target"
}

kill_processes_under_app_home() {
    # 根据命令行中 APP_HOME 路径清理残留服务进程，安装/删除时作为兜底。
    self_pid="$$"
    ps w 2>/dev/null | grep "$APP_HOME" | grep -E 'encoder_main\.sh|app_service\.sh|voice\.sh|led\.sh' | grep -v grep | while read -r pid _; do
        [ -n "$pid" ] || continue
        [ "$pid" = "$self_pid" ] && continue
        kill "$pid" 2>/dev/null
    done
}
