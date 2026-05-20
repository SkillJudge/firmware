#!/bin/sh

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

ensure_mqtt_tools() {
    require_command jq || return 1
    require_command mosquitto_pub || return 1
    require_command mosquitto_sub || return 1
}

mqtt_pub_json() {
    topic="$1"
    payload="$2"

    mosquitto_pub \
        -h "$MQTT_HOST" \
        -p "$MQTT_PORT" \
        -u "$MQTT_USER" \
        -P "$MQTT_PASS" \
        -q "$MQTT_QOS" \
        -t "$topic" \
        -m "$payload"
}

mqtt_sub_forever_with_topic() {
    topic="$1"

    mosquitto_sub \
        -h "$MQTT_HOST" \
        -p "$MQTT_PORT" \
        -u "$MQTT_USER" \
        -P "$MQTT_PASS" \
        -q "$MQTT_QOS" \
        -v \
        -t "$topic"
}

mqtt_sub_once() {
    topic="$1"
    timeout_sec="$2"
    count="$3"

    [ -n "$timeout_sec" ] || timeout_sec=10
    [ -n "$count" ] || count=1

    mosquitto_sub \
        -h "$MQTT_HOST" \
        -p "$MQTT_PORT" \
        -u "$MQTT_USER" \
        -P "$MQTT_PASS" \
        -q "$MQTT_QOS" \
        -W "$timeout_sec" \
        -C "$count" \
        -t "$topic"
}
