#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/state.sh"

# Heartbeat/reporting keeps SOC percent and the latest valid VCELL voltage in mV.
# Manual status output also prints both values for field debugging.
: "${BATTERY_REFRESH_ENABLED:=true}"
: "${BATTERY_I2C_BUS:=1}"
: "${BATTERY_I2C_ADDR:=0x36}"
: "${BATTERY_VCELL_REG:=0x02}"
: "${BATTERY_SOC_REG:=0x04}"
: "${BATTERY_CRATE_REG:=0x16}"
: "${BATTERY_CHARGE_GPIO_I2C_BUS:=${LED_I2C_BUS:-1}}"
: "${BATTERY_CHARGE_GPIO_I2C_ADDR:=${LED_I2C_ADDR:-0x20}}"
: "${BATTERY_CHRG_GPIO_MASK:=0x01}"
: "${BATTERY_STDBY_GPIO_MASK:=0x02}"
: "${BATTERY_PROTECT_GPIO_MASK:=0x07}"
: "${BATTERY_CHARGING_THRESHOLD_RAW:=5}"
: "${BATTERY_DISCHARGING_THRESHOLD_RAW:=-5}"
: "${BATTERY_LOW_SHUTDOWN_ENABLED:=true}"
: "${BATTERY_LOW_SHUTDOWN_THRESHOLD_MV:=3200}"
: "${BATTERY_LOW_SHUTDOWN_DELAY_SEC:=2}"
: "${BATTERY_LOW_SHUTDOWN_COMMAND:=poweroff}"
: "${BATTERY_LOW_SHUTDOWN_OWNER_FILE:=/tmp/alarm_monitor_power_shutdown.owner}"
: "${BATTERY_LOW_SHUTDOWN_OWNER_MAX_AGE_SEC:=5}"

battery_swap_word() {
    raw="$1"
    case "$raw" in
        0x[0-9a-fA-F][0-9a-fA-F][0-9a-fA-F][0-9a-fA-F])
            ;;
        *)
            return 1
            ;;
    esac

    word=$((raw))
    printf '%s\n' "$(( ((word & 255) << 8) | ((word >> 8) & 255) ))"
}

battery_read_percent() {
    command_exists i2cget || return 1

    # SOC is a fixed-point value: high byte is integer percent, low byte is fraction.
    raw_soc=$(i2cget -y "$BATTERY_I2C_BUS" "$BATTERY_I2C_ADDR" "$BATTERY_SOC_REG" w 2>/dev/null) || return 1
    soc=$(battery_swap_word "$raw_soc") || return 1
    percent=$((soc / 256))

    [ "$percent" -lt 0 ] 2>/dev/null && percent=0
    [ "$percent" -gt 100 ] 2>/dev/null && percent=100

    printf '%s\n' "$percent"
}

battery_read_voltage_mv() {
    command_exists i2cget || return 1

    # VCELL is a 12-bit battery voltage reading. One count is 1.25mV.
    raw_vcell=$(i2cget -y "$BATTERY_I2C_BUS" "$BATTERY_I2C_ADDR" "$BATTERY_VCELL_REG" w 2>/dev/null) || return 1
    vcell=$(battery_swap_word "$raw_vcell") || return 1
    vcell_count=$((vcell >> 4))
    voltage_mv=$(((vcell_count * 125 + 50) / 100))

    printf '%s\n' "$voltage_mv"
}

battery_read_crate_raw() {
    command_exists i2cget || return 1

    # CRATE is signed raw charge/discharge rate. Positive means charging.
    raw_crate=$(i2cget -y "$BATTERY_I2C_BUS" "$BATTERY_I2C_ADDR" "$BATTERY_CRATE_REG" w 2>/dev/null) || return 1
    crate=$(battery_swap_word "$raw_crate") || return 1
    [ "$crate" -ge 32768 ] && crate=$((crate - 65536))

    printf '%s\n' "$crate"
}

battery_charge_gpio_lock_acquire() {
    [ -n "$LED_I2C_LOCK_DIR" ] || return 0
    ensure_layout
    lock_try=0

    while ! mkdir "$LED_I2C_LOCK_DIR" 2>/dev/null; do
        lock_owner=$(cat "$LED_I2C_LOCK_DIR/pid" 2>/dev/null)
        if [ -n "$lock_owner" ] && ! kill -0 "$lock_owner" 2>/dev/null; then
            rm -rf "$LED_I2C_LOCK_DIR"
            continue
        fi

        lock_try=$((lock_try + 1))
        if [ "$lock_try" -ge 5 ]; then
            log_warn_tag "BATTERY" "GPIO I2C lock timeout"
            return 1
        fi
        sleep 1
    done

    printf '%s\n' "$$" > "$LED_I2C_LOCK_DIR/pid"
}

battery_charge_gpio_lock_release() {
    [ -n "$LED_I2C_LOCK_DIR" ] || return 0
    [ -d "$LED_I2C_LOCK_DIR" ] || return 0
    lock_owner=$(cat "$LED_I2C_LOCK_DIR/pid" 2>/dev/null)
    [ "$lock_owner" = "$$" ] || return 0
    rm -rf "$LED_I2C_LOCK_DIR"
}

battery_read_charge_gpio_raw() {
    command_exists i2cget || return 1
    command_exists i2cset || return 1

    battery_charge_gpio_lock_acquire || return 1
    raw_gpio=$(i2cget -y "$BATTERY_CHARGE_GPIO_I2C_BUS" "$BATTERY_CHARGE_GPIO_I2C_ADDR" 2>/dev/null)
    gpio_rc=$?
    if [ "$gpio_rc" != "0" ]; then
        battery_charge_gpio_lock_release
        return 1
    fi

    case "$raw_gpio" in
        0x[0-9a-fA-F][0-9a-fA-F])
            ;;
        *)
            battery_charge_gpio_lock_release
            return 1
            ;;
    esac

    # PCF8574 inputs must be written as 1 before reading. Keep P0/P1/P2 released,
    # otherwise a previous low latch can make CHRG/STDBY look permanently low.
    release_gpio=$((raw_gpio | BATTERY_PROTECT_GPIO_MASK))
    release_hex=$(printf '0x%02x' "$release_gpio")
    i2cset -y "$BATTERY_CHARGE_GPIO_I2C_BUS" "$BATTERY_CHARGE_GPIO_I2C_ADDR" "$release_hex" >/dev/null 2>&1 || {
        battery_charge_gpio_lock_release
        return 1
    }
    sleep 1

    raw_gpio=$(i2cget -y "$BATTERY_CHARGE_GPIO_I2C_BUS" "$BATTERY_CHARGE_GPIO_I2C_ADDR" 2>/dev/null)
    gpio_rc=$?
    battery_charge_gpio_lock_release
    [ "$gpio_rc" = "0" ] || return 1

    case "$raw_gpio" in
        0x[0-9a-fA-F][0-9a-fA-F])
            ;;
        *)
            return 1
            ;;
    esac

    printf '%s\n' "$((raw_gpio))"
}

battery_gpio_level() {
    gpio_value="$1"
    gpio_mask="$2"

    if [ "$((gpio_value & gpio_mask))" = "0" ]; then
        printf '0\n'
    else
        printf '1\n'
    fi
}

battery_interpret_charging_levels() {
    chrg_level="$1"
    stdby_level="$2"

    case "${chrg_level}:${stdby_level}" in
        0:1)
            printf 'true\n'
            ;;
        1:0|1:1)
            printf 'false\n'
            ;;
        0:0)
            # CHRG and STDBY are both active-low, so both low is not a valid
            # charging-state decision. Fall back to CRATE instead.
            return 2
            ;;
        *)
            return 1
            ;;
    esac
}

battery_read_charging_from_gpio() {
    gpio=$(battery_read_charge_gpio_raw) || return 1
    chrg_level=$(battery_gpio_level "$gpio" "$BATTERY_CHRG_GPIO_MASK")
    stdby_level=$(battery_gpio_level "$gpio" "$BATTERY_STDBY_GPIO_MASK")

    # CHRG/STDBY are active-low status pins. Use only unambiguous combinations.
    battery_interpret_charging_levels "$chrg_level" "$stdby_level"
}

battery_refresh_charging_state() {
    gpio_charging=""
    if gpio_charging=$(battery_read_charging_from_gpio); then
        gpio_rc=0
    else
        gpio_rc=$?
    fi

    if [ "$gpio_rc" = "0" ] && [ -n "$gpio_charging" ]; then
        state_set_charging "$gpio_charging"
        log_debug_tag "BATTERY" "charge GPIO is_charging=$gpio_charging"
        return 0
    fi

    log_debug_tag "BATTERY" "charge GPIO unavailable/ambiguous rc=$gpio_rc, fallback to CRATE"

    crate=$(battery_read_crate_raw) || {
        log_debug_tag "BATTERY" "CRATE read failed, keep is_charging=$(state_get_charging)"
        return 0
    }

    charging=$(state_get_charging)
    case "$charging" in
        true|false)
            ;;
        *)
            charging=false
            ;;
    esac

    # Keep the previous state around zero to avoid toggling at full charge or under a changing load.
    if [ "$crate" -ge "$BATTERY_CHARGING_THRESHOLD_RAW" ]; then
        charging=true
    elif [ "$crate" -le "$BATTERY_DISCHARGING_THRESHOLD_RAW" ]; then
        charging=false
    fi

    state_set_charging "$charging"
    log_debug_tag "BATTERY" "CRATE raw=$crate is_charging=$charging"
    return 0
}

battery_is_uint() {
    case "$1" in
        ''|*[!0-9]*)
            return 1
            ;;
    esac
    return 0
}

battery_schedule_low_voltage_shutdown() {
    voltage_mv="$1"
    threshold_mv="$2"
    delay_sec="$3"

    battery_is_uint "$delay_sec" || delay_sec=0
    [ -n "$BATTERY_LOW_SHUTDOWN_COMMAND" ] || {
        log_error_tag "BATTERY" "low voltage shutdown command empty voltage_mv=$voltage_mv threshold_mv=$threshold_mv"
        return 1
    }

    log_error_tag "BATTERY" "low voltage shutdown scheduled voltage_mv=$voltage_mv threshold_mv=$threshold_mv delay=${delay_sec}s command=$BATTERY_LOW_SHUTDOWN_COMMAND"
    (
        sync 2>/dev/null || true
        sleep "$delay_sec"
        sh -c "$BATTERY_LOW_SHUTDOWN_COMMAND"
    ) &
}

battery_check_low_voltage_shutdown() {
    [ "$BATTERY_LOW_SHUTDOWN_ENABLED" = "true" ] || return 0

    voltage_mv="$1"
    threshold_mv="$BATTERY_LOW_SHUTDOWN_THRESHOLD_MV"

    battery_is_uint "$voltage_mv" || return 0
    if ! battery_is_uint "$threshold_mv"; then
        log_warn_tag "BATTERY" "invalid low shutdown threshold: $threshold_mv"
        return 0
    fi

    # 报警监测进程存活时，由它负责“先报警、等待 ACK、再关机”。若监测
    # 进程退出，owner PID 会失效，本脚本立即恢复原有的 3.2V 安全关机兜底。
    owner_pid=$(cat "$BATTERY_LOW_SHUTDOWN_OWNER_FILE" 2>/dev/null)
    owner_mtime=$(stat -c %Y "$BATTERY_LOW_SHUTDOWN_OWNER_FILE" 2>/dev/null)
    now_sec=$(date '+%s')
    if battery_is_uint "$owner_pid" && battery_is_uint "$owner_mtime" && battery_is_uint "$now_sec" && \
        [ $((now_sec - owner_mtime)) -le "$BATTERY_LOW_SHUTDOWN_OWNER_MAX_AGE_SEC" ] && kill -0 "$owner_pid" 2>/dev/null; then
        if [ -r "/proc/$owner_pid/cmdline" ] && tr '\000' ' ' < "/proc/$owner_pid/cmdline" | grep -F 'alarm_monitor.sh' >/dev/null 2>&1; then
            log_debug_tag "BATTERY" "low voltage shutdown delegated to alarm monitor pid=$owner_pid"
            return 0
        fi
    fi

    if [ "$voltage_mv" -lt "$threshold_mv" ]; then
        battery_schedule_low_voltage_shutdown "$voltage_mv" "$threshold_mv" "$BATTERY_LOW_SHUTDOWN_DELAY_SEC"
    fi
}

battery_refresh_state() {
    [ "$BATTERY_REFRESH_ENABLED" = "true" ] || return 0

    percent=$(battery_read_percent) || {
        log_debug_tag "BATTERY" "read failed, keep battery=$(state_get_battery)"
        percent=""
    }

    if [ -n "$percent" ]; then
        state_set_battery "$percent"
        log_debug_tag "BATTERY" "battery=$percent"
    fi

    voltage_mv=$(battery_read_voltage_mv) || {
        log_debug_tag "BATTERY" "voltage read failed, keep voltage_mv=$(state_get_battery_voltage_mv)"
        voltage_mv=""
    }

    if [ -n "$voltage_mv" ]; then
        state_set_battery_voltage_mv "$voltage_mv"
        log_debug_tag "BATTERY" "voltage_mv=$voltage_mv"
        battery_check_low_voltage_shutdown "$voltage_mv"
    fi

    battery_refresh_charging_state
    return 0
}

battery_print_status() {
    percent=$(battery_read_percent) || return 1
    voltage_mv=$(battery_read_voltage_mv) || return 1

    printf 'percent=%s%%\n' "$percent"
    printf 'voltage=%smV\n' "$voltage_mv"
    if gpio=$(battery_read_charge_gpio_raw 2>/dev/null); then
        chrg_level=$(battery_gpio_level "$gpio" "$BATTERY_CHRG_GPIO_MASK")
        stdby_level=$(battery_gpio_level "$gpio" "$BATTERY_STDBY_GPIO_MASK")
        if is_charging=$(battery_interpret_charging_levels "$chrg_level" "$stdby_level" 2>/dev/null); then
            :
        else
            is_charging=unknown
        fi
        printf 'gpio=0x%02x\n' "$gpio"
        printf 'gpio0_chrg=%s\n' "$chrg_level"
        printf 'gpio1_stdby=%s\n' "$stdby_level"
        printf 'is_charging=%s\n' "$is_charging"
    else
        printf 'gpio=unknown\n'
        printf 'is_charging=unknown\n'
    fi
}

if [ "$(basename "$0")" = "battery.sh" ]; then
    case "${1:-status}" in
        refresh)
            battery_refresh_state
            state_get_battery
            ;;
        read)
            battery_read_percent
            ;;
        voltage)
            battery_read_voltage_mv
            ;;
        gpio)
            gpio=$(battery_read_charge_gpio_raw) || exit 1
            printf 'gpio=0x%02x\n' "$gpio"
            printf 'gpio0_chrg=%s\n' "$(battery_gpio_level "$gpio" "$BATTERY_CHRG_GPIO_MASK")"
            printf 'gpio1_stdby=%s\n' "$(battery_gpio_level "$gpio" "$BATTERY_STDBY_GPIO_MASK")"
            ;;
        status)
            battery_print_status
            ;;
        charging)
            battery_read_charging_from_gpio || {
                rc=$?
                [ "$rc" = "2" ] && {
                    printf 'unknown\n'
                    exit 2
                }
                exit 1
            }
            ;;
        crate)
            battery_read_crate_raw
            ;;
        *)
            echo "usage: sh battery.sh {status|read|voltage|gpio|refresh|charging|crate}" >&2
            exit 1
            ;;
    esac
fi
