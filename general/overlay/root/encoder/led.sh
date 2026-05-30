#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

# Only these three bits belong to LEDs. Preserve every other expander bit.
LED_CONTROL_MASK=$((LED_MONO_MASK | LED_STREAM_MASK | LED_UPLOAD_MASK))
LED_UPLOAD_TOKEN=""

led_require_i2c_tools() {
    command_exists i2cget && command_exists i2cset
}

led_lock_acquire() {
    ensure_layout
    led_lock_try=0

    while ! mkdir "$LED_I2C_LOCK_DIR" 2>/dev/null; do
        led_lock_owner=$(cat "$LED_I2C_LOCK_DIR/pid" 2>/dev/null)
        if [ -n "$led_lock_owner" ] && ! kill -0 "$led_lock_owner" 2>/dev/null; then
            rm -rf "$LED_I2C_LOCK_DIR"
            continue
        fi

        led_lock_try=$((led_lock_try + 1))
        if [ "$led_lock_try" -ge 5 ]; then
            log_warn_tag "LED" "I2C lock timeout"
            return 1
        fi
        sleep 1
    done

    printf '%s\n' "$$" > "$LED_I2C_LOCK_DIR/pid"
}

led_lock_release() {
    [ -d "$LED_I2C_LOCK_DIR" ] || return 0
    led_lock_owner=$(cat "$LED_I2C_LOCK_DIR/pid" 2>/dev/null)
    [ "$led_lock_owner" = "$$" ] || return 0
    rm -rf "$LED_I2C_LOCK_DIR"
}

led_read_value() {
    led_raw=$(i2cget -y "$LED_I2C_BUS" "$LED_I2C_ADDR" 2>/dev/null) || return 1
    case "$led_raw" in
        0x[0-9a-fA-F][0-9a-fA-F])
            ;;
        *)
            return 1
            ;;
    esac

    printf '%s\n' "$((led_raw))"
}

led_write_value() {
    led_hex=$(printf '0x%02x' "$1")
    i2cset -y "$LED_I2C_BUS" "$LED_I2C_ADDR" "$led_hex" >/dev/null 2>&1
}

led_update_bits() {
    led_set_mask="$1"
    led_clear_mask="$2"

    [ "$LED_ENABLED" = "true" ] || return 0
    if ! led_require_i2c_tools; then
        log_warn_tag "LED" "required I2C command missing"
        return 1
    fi

    led_lock_acquire || return 1
    led_current=$(led_read_value)
    led_rc=$?

    if [ "$led_rc" = "0" ]; then
        led_next=$(( (led_current | led_set_mask) & (~led_clear_mask & 255) ))
        if [ "$led_next" != "$led_current" ]; then
            led_write_value "$led_next"
            led_rc=$?
        fi
    fi

    led_lock_release
    if [ "$led_rc" != "0" ]; then
        log_warn_tag "LED" "I2C update failed set_mask=$led_set_mask clear_mask=$led_clear_mask"
        return 1
    fi
    return 0
}

led_all_on() { led_update_bits "$LED_CONTROL_MASK" 0; }
led_all_off() { led_update_bits 0 "$LED_CONTROL_MASK"; }
led_stream_on() { led_update_bits "$LED_STREAM_MASK" 0; }
led_stream_off() { led_update_bits 0 "$LED_STREAM_MASK"; }
led_record_on() { led_update_bits "$LED_MONO_MASK" 0; }
led_record_off() { led_update_bits 0 "$LED_MONO_MASK"; }
led_upload_on() { led_update_bits "$LED_UPLOAD_MASK" 0; }
led_upload_off() { led_update_bits 0 "$LED_UPLOAD_MASK"; }

led_upload_tokens_prepare() {
    ensure_layout
    mkdir -p "$LED_UPLOAD_TOKEN_DIR"
}

led_upload_tokens_clear() {
    led_upload_tokens_prepare
    rm -f "$LED_UPLOAD_TOKEN_DIR"/upload_* 2>/dev/null
}

led_upload_has_tokens() {
    led_upload_tokens_prepare
    for led_token_file in "$LED_UPLOAD_TOKEN_DIR"/upload_*; do
        [ -f "$led_token_file" ] && return 0
    done
    return 1
}

led_upload_blink_worker() {
    led_upload_tokens_prepare
    if ! claim_pidfile "$LED_UPLOAD_BLINK_PID_FILE"; then
        exit 0
    fi

    trap 'release_pidfile_if_owner "$LED_UPLOAD_BLINK_PID_FILE"' EXIT
    trap 'exit 0' INT TERM
    log_debug_tag "LED" "upload blink worker start"

    while true; do
        if led_upload_has_tokens; then
            led_upload_on
            sleep "$LED_UPLOAD_ON_SEC"
            led_upload_off
            sleep "$LED_UPLOAD_OFF_SEC"
        else
            sleep "$LED_IDLE_POLL_SEC"
        fi
    done
}

led_upload_worker_start() {
    [ "$LED_ENABLED" = "true" ] || return 0
    led_upload_tokens_prepare
    if is_pid_running_file "$LED_UPLOAD_BLINK_PID_FILE"; then
        return 0
    fi

    sh "$APP_HOME/led.sh" upload_blink_worker >/dev/null 2>&1 &
}

led_upload_begin() {
    LED_UPLOAD_TOKEN=""
    [ "$LED_ENABLED" = "true" ] || return 0

    led_upload_tokens_prepare
    led_token_suffix=0
    while true; do
        led_token_path="${LED_UPLOAD_TOKEN_DIR}/upload_$$.$(raw_now_sec).${led_token_suffix}"
        [ -e "$led_token_path" ] || break
        led_token_suffix=$((led_token_suffix + 1))
    done

    : > "$led_token_path" || {
        log_warn_tag "LED" "upload token create failed path=$led_token_path"
        return 1
    }

    LED_UPLOAD_TOKEN="$led_token_path"
    led_upload_on || true
    led_upload_worker_start
}

led_upload_end() {
    led_token_path="$1"
    [ -n "$led_token_path" ] || return 0
    rm -f "$led_token_path"
    if ! led_upload_has_tokens; then
        led_upload_off || true
    fi
}

led_runtime_start() {
    led_upload_tokens_clear
    led_all_off
    led_upload_worker_start
}

led_runtime_reset_idle() {
    stop_pidfile_process "$LED_UPLOAD_BLINK_PID_FILE"
    led_upload_tokens_clear
    led_all_off
    led_upload_worker_start
}

led_runtime_stop() {
    stop_pidfile_process "$LED_UPLOAD_BLINK_PID_FILE"
    led_upload_tokens_clear
    led_all_on
}

if [ "$(basename "$0")" = "led.sh" ]; then
    case "${1:-}" in
        all_on) led_all_on ;;
        all_off) led_all_off ;;
        stream_on) led_stream_on ;;
        stream_off) led_stream_off ;;
        record_on) led_record_on ;;
        record_off) led_record_off ;;
        upload_on) led_upload_on ;;
        upload_off) led_upload_off ;;
        upload_blink_worker) led_upload_blink_worker ;;
        *)
            echo "usage: sh led.sh {all_on|all_off|stream_on|stream_off|record_on|record_off|upload_on|upload_off|upload_blink_worker}" >&2
            exit 1
            ;;
    esac
fi
