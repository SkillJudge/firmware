#!/bin/sh

# 消息收发工具封装层，内部使用 MQTT。
# 统一 mosquitto_pub/sub 参数，业务层只需要传 topic 和 payload。
SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
. "$SCRIPT_DIR/common.sh"

ensure_mqtt_tools() {
    # 板端必须有 jq 和 mosquitto 工具，否则无法解析协议或收发 MQTT。
    require_command jq || return 1
    require_command mosquitto_pub || return 1
    require_command mosquitto_sub || return 1
}

mqtt_pub_json() {
    # 发布 JSON 消息，连接参数统一来自 config.sh。
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
    # 常驻订阅，-v 会输出 "topic payload"，listener 依赖这个格式拆解消息。
    topic="$1"

    exec mosquitto_sub \
        -h "$MQTT_HOST" \
        -p "$MQTT_PORT" \
        -u "$MQTT_USER" \
        -P "$MQTT_PASS" \
        -q "$MQTT_QOS" \
        -v \
        -t "$topic"
}

mqtt_sub_once() {
    # 临时订阅固定条数消息，注册等待 ACK 时使用。
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
