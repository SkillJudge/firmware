#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/state.sh"

: "${BATTERY_REFRESH_ENABLED:=true}"
: "${BATTERY_I2C_BUS:=1}"
: "${BATTERY_I2C_ADDR:=0x36}"
: "${BATTERY_SOC_REG:=0x04}"
: "${BATTERY_CRATE_REG:=0x16}"
: "${BATTERY_CHARGING_THRESHOLD_RAW:=5}"
: "${BATTERY_DISCHARGING_THRESHOLD_RAW:=-5}"

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

    raw_soc=$(i2cget -y "$BATTERY_I2C_BUS" "$BATTERY_I2C_ADDR" "$BATTERY_SOC_REG" w 2>/dev/null) || return 1
    soc=$(battery_swap_word "$raw_soc") || return 1
    percent=$((soc / 256))

    [ "$percent" -lt 0 ] 2>/dev/null && percent=0
    [ "$percent" -gt 100 ] 2>/dev/null && percent=100

    printf '%s\n' "$percent"
}

battery_read_crate_raw() {
    command_exists i2cget || return 1

    raw_crate=$(i2cget -y "$BATTERY_I2C_BUS" "$BATTERY_I2C_ADDR" "$BATTERY_CRATE_REG" w 2>/dev/null) || return 1
    crate=$(battery_swap_word "$raw_crate") || return 1
    [ "$crate" -ge 32768 ] && crate=$((crate - 65536))

    printf '%s\n' "$crate"
}

battery_refresh_charging_state() {
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

    battery_refresh_charging_state
    return 0
}

if [ "$(basename "$0")" = "battery.sh" ]; then
    case "${1:-read}" in
        refresh)
            battery_refresh_state
            state_get_battery
            ;;
        read)
            battery_read_percent
            ;;
        charging)
            battery_read_crate_raw
            ;;
        *)
            echo "usage: sh battery.sh {read|refresh|charging}" >&2
            exit 1
            ;;
    esac
fi
