#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/config.sh"

ensure_layout() {
    mkdir -p "$WORKDIR" "$STATE_DIR" "$LOG_DIR" "$TMP_DIR" "$RECORD_NAMED_LOCAL_DIR" "$CAPTURE_LOCAL_DIR" "$AUDIO_LOCAL_DIR"
    [ -f "$MSGID_FILE" ] || echo 1000 > "$MSGID_FILE"
}

now_str() {
    date '+%F %T'
}

raw_now_sec() {
    date '+%s'
}

raw_now_ms() {
    echo $(( $(raw_now_sec) * 1000 ))
}

script_name() {
    basename "${0:-sh}"
}

print_title() {
    ensure_layout
    printf '===== %s %s =====\n' "$1" "$DEVICE_VERSION"
}

log_write() {
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
    log_write "INFO" "$@"
}

log_info_tag() {
    tag="$1"
    shift
    log_write "INFO" "[$tag] $*"
}

log_debug() {
    [ "$LOG_VERBOSE" = "true" ] || return 0
    log_write "DEBUG" "$@"
}

log_debug_tag() {
    [ "$LOG_VERBOSE" = "true" ] || return 0
    tag="$1"
    shift
    log_write "DEBUG" "[$tag] $*"
}

log_warn() {
    log_write "WARN" "$@"
}

log_warn_tag() {
    tag="$1"
    shift
    log_write "WARN" "[$tag] $*"
}

log_error() {
    log_write "ERROR" "$@"
}

log_error_tag() {
    tag="$1"
    shift
    log_write "ERROR" "[$tag] $*"
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

shell_quote() {
    value="$1"
    escaped=$(printf '%s' "$value" | sed "s/'/'\\\\''/g")
    printf "'%s'\n" "$escaped"
}

require_command() {
    if command_exists "$1"; then
        return 0
    fi

    log_error "required command missing: $1"
    return 1
}

is_valid_device_id() {
    value="$1"
    [ -n "$value" ] || return 1

    sanitized=$(printf '%s' "$value" | sed 's/[^A-Za-z0-9._-]/_/g')
    [ "$sanitized" = "$value" ]
}

ensure_device_id_configured() {
    if [ "$DEVICE_ID_CONFIGURED" != "true" ] || [ -z "$DEVICE_ID" ]; then
        log_error "device id missing: set ENCODER_DEVICE_ID or DEVICE_ID_DEFAULT before starting encoder"
        return 1
    fi

    if ! is_valid_device_id "$DEVICE_ID"; then
        log_error "device id invalid: DEVICE_ID=$DEVICE_ID allowed=A-Za-z0-9._-"
        return 1
    fi

    return 0
}

next_msgid() {
    ensure_layout
    n=$(cat "$MSGID_FILE" 2>/dev/null)
    [ -n "$n" ] || n=1000
    n=$((n + 1))
    printf '%s\n' "$n" > "$MSGID_FILE"
    printf '%s\n' "$n"
}

get_ip_addr() {
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

is_pid_running_file() {
    pidfile="$1"
    [ -f "$pidfile" ] || return 1

    pid=$(cat "$pidfile" 2>/dev/null)
    [ -n "$pid" ] || return 1

    kill -0 "$pid" 2>/dev/null
}

claim_pidfile() {
    pidfile="$1"
    current_pid="$$"

    if [ -f "$pidfile" ]; then
        existing_pid=$(cat "$pidfile" 2>/dev/null)
        if [ -n "$existing_pid" ] && [ "$existing_pid" != "$current_pid" ] && kill -0 "$existing_pid" 2>/dev/null; then
            return 1
        fi
    fi

    printf '%s\n' "$current_pid" > "$pidfile"
    return 0
}

release_pidfile_if_owner() {
    pidfile="$1"
    current_pid="$$"

    [ -f "$pidfile" ] || return 0
    existing_pid=$(cat "$pidfile" 2>/dev/null)
    [ "$existing_pid" = "$current_pid" ] || return 0

    rm -f "$pidfile"
}

stop_pidfile_process() {
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

file_mtime_sec() {
    stat -c %Y "$1" 2>/dev/null || echo 0
}

file_size_bytes() {
    wc -c < "$1" 2>/dev/null | tr -d '[:space:]'
}

now_with_format() {
    date "+$1"
}

sanitize_name_part() {
    value="$1"
    sanitized=$(printf '%s' "$value" | sed 's/[^A-Za-z0-9._-]/_/g')
    [ -n "$sanitized" ] || sanitized="unknown"
    printf '%s\n' "$sanitized"
}

list_record_files() {
    [ -d "$RECORD_SEARCH_ROOT" ] || return 0
    find "$RECORD_SEARCH_ROOT" -type f -name '*.mp4' 2>/dev/null | sort
}

safe_remove_path() {
    target="$1"
    [ -e "$target" ] || return 0
    rm -rf "$target"
}

kill_processes_under_app_home() {
    self_pid="$$"
    ps w 2>/dev/null | grep "$APP_HOME" | grep -E 'encoder_main\.sh|app_service\.sh' | grep -v grep | while read -r pid _; do
        [ -n "$pid" ] || continue
        [ "$pid" = "$self_pid" ] && continue
        kill "$pid" 2>/dev/null
    done
}
