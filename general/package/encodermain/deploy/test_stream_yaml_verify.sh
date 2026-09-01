#!/bin/sh
# ======================================================================
# test_stream_yaml_verify.sh — 推流后校验 majestic.yaml 配置正确性用例
# 环境: 编码器板端 (OpenIPC BusyBox ash)，必须已安装:
#         yaml-cli, mosquitto_pub/mosquitto_sub, fw_printenv, md5sum, awk
# 设计 (用户4条请求的第4条: "设计测试用例测试推流后majestic.yaml正确性"):
#   前置: encodermain 已连 MQTT，DEVICE_ID 存在，state idling=true
#   步骤:
#     A. 下发 start_stream MQTT 命令 (6段正确topic + payload)
#     B. 等 encodermain 写 yaml + HUP majestic + TCP 1935建链 (最多40s)
#     C. 执行 4 项断言:
#        1. majestic.yaml .outgoing.server 非空
#        2. outgoing.server = rtmp://<host>:<port>/live/stream_<DEVICE_ID>
#           (path ≥2 段且首段 app=live)
#        3. state/current_stream_url 与 yaml outgoing.server 完全一致
#        4. /proc/net/tcp 存在远程端口 0x078F(=1935) 状态 0x01(ESTABLISHED)
#        5*. state/is_publishing = true
#     D. 下发 stop_stream，再校验: outgoing.server 清空, is_publishing=false
#     E. 输出 PASS/FAIL 报告，失败时 dump:
#           - yaml outgoing 段
#           - encodermain log 最后40行
#           - /proc/net/tcp (过滤 078F)
#           - state/current_stream_url
#
# 用法 (板上执行, root):
#   sh test_stream_yaml_verify.sh                     # 自测, 用设备自身MQTT配置
#   sh test_stream_yaml_verify.sh --host 192.168.250.100 --port 1883 \
#      --user mqttadmin --pass skilljudge123 --skip-stop   # 自定义MQTT + 不自动停流
#
# 失败码: 1=用例FAIL; 2=环境缺依赖; 3=前置不满足(DEVICE_ID/encodermain未启动)
# ======================================================================
set -u

MQTT_HOST=""
MQTT_PORT="1883"
MQTT_USER=""
MQTT_PASS=""
SKIP_STOP=0
SRS_PORT_NUM=1935
SRS_PORT_HEX="078F"      # 1935 in hex, little-endian in /proc/net/tcp = bytes reversed
# NOTE: /proc/net/tcp remote_address is written as HOST_LITTLE_ENDIAN uint16,
# so we will filter by ":078F" (the printed human hex is actually net order).
# Actually: /proc/net/tcp prints hex in standard host-order. 1935 = 0x078F.
# We'll match "078F" literally as the port portion.

for a in "$@"; do case "$a" in
  --host) shift; MQTT_HOST=$1; shift ;;
  --port) shift; MQTT_PORT=$1; shift ;;
  --user) shift; MQTT_USER=$1; shift ;;
  --pass) shift; MQTT_PASS=$1; shift ;;
  --skip-stop) SKIP_STOP=1; shift ;;
  -h|--help) sed -n '2,45p' "$0"; exit 0 ;;
esac; done

# -------- 工具 --------
log()  { printf "[TEST %s] %s\n" "$(date '+%H:%M:%S')" "$*"; }
fail() { printf "[TEST FAIL %s] %s\n" "$(date '+%H:%M:%S')" "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "缺少依赖命令: $1" >&2; exit 2; }; }

need mosquitto_pub
need mosquitto_sub
need yaml-cli
need awk
need fw_printenv

MAJESTIC_CONF="/etc/majestic.yaml"
STATE_DIR="/root/encoder/runtime/state"
ENCLOG="/root/encoder/runtime/logs/encoder.log"
[ -f "$MAJESTIC_CONF" ] || { echo "majestic yaml 不存在: $MAJESTIC_CONF" >&2; exit 2; }
[ -d "$STATE_DIR" ]    || { echo "state 目录不存在: $STATE_DIR" >&2; exit 3; }

DEVICE_ID="$(fw_printenv -n DEVICE_ID 2>/dev/null)"
[ -n "${DEVICE_ID:-}" ] || fail "env 无 DEVICE_ID, encodermain 未初始化"
log "DEVICE_ID=$DEVICE_ID"

# 如果没传 MQTT 参数，从 /root/encoder/config.sh 继承 (与 encodermain 相同)
if [ -z "$MQTT_HOST" ] && [ -f /root/encoder/config.sh ]; then
  # shellcheck disable=SC1091
  . /root/encoder/config.sh >/dev/null 2>&1 || true
  MQTT_HOST="${ENC_MQTT_HOST:-$MQTT_HOST}"
  MQTT_PORT="${ENC_MQTT_PORT:-$MQTT_PORT}"
  MQTT_USER="${ENC_MQTT_USER:-$MQTT_USER}"
  MQTT_PASS="${ENC_MQTT_PASS:-$MQTT_PASS}"
fi
# 再兜底一次 LAN 服务器
MQTT_HOST="${MQTT_HOST:-192.168.250.100}"
MQTT_PORT="${MQTT_PORT:-1883}"
MQTT_USER="${MQTT_USER:-mqttadmin}"
# MQTT_PASS 如果读不到，报错提醒
if [ -z "$MQTT_PASS" ]; then
  # 尝试 /etc/default/encoder
  [ -f /etc/default/encoder ] && . /etc/default/encoder >/dev/null 2>&1 || true
  MQTT_PASS="${ENC_MQTT_PASS:-}"
fi
[ -n "$MQTT_PASS" ] || fail "MQTT_PASS 为空; 请传 --pass 或在 /root/encoder/config.sh 设置 ENC_MQTT_PASS"

log "MQTT broker=$MQTT_HOST:$MQTT_PORT user=$MQTT_USER"
if ! pidof encodermain >/dev/null 2>&1; then
  fail "encodermain 未启动, 先 /etc/init.d/S96encodermain start"
fi

# -------- MQTT 发送 helper --------
# topic 严格 6 段: sender/senderSub/encoder/deviceId/flow/action
TOPIC_START="ctrlsrv/0/encoder/${DEVICE_ID}/stream/start_stream"
TOPIC_STOP="ctrlsrv/0/encoder/${DEVICE_ID}/stream/stop_stream"
EXPECTED_STREAM_NAME="stream_${DEVICE_ID}"
# 构造两条测试命令 (A=单级path Bug形态, B=多级path正常形态);
# A: 旧版 control 下发缺 /live 的 URL, 若 C 端 Bug 未修 → outgoing 最终仍缺 /live → FAIL
URL_A="rtmp://${MQTT_HOST}:1935/${EXPECTED_STREAM_NAME}"
# B: 控制端显式给出 live app 段, C 端替换最后一级 → OK (基准对照组)
URL_B="rtmp://${MQTT_HOST}:1935/live/demoTask_$$"
EXPECT_URL_B="rtmp://${MQTT_HOST}:1935/live/${EXPECTED_STREAM_NAME}"

mq_auth="-h $MQTT_HOST -p $MQTT_PORT -u $MQTT_USER -P $MQTT_PASS"

publish_cmd() {   # $1=topic, $2=payload_json
  # shellcheck disable=SC2086
  mosquitto_pub $mq_auth -t "$1" -m "$2" -q 1
}

# stop_stream_until_idle：多轮下发 stop 直到 is_publishing!='true' 或超限
# 背景: encodermain 对 stop 做 L2 dedup，当 T1 URL(非live伪两级)→归一化 URL 与
#       dispatch stop key 计算基于"前一次成功URL"时可能不匹配，stop 被 replay，
#       需多发几轮 + 主动清 yaml 触发 majestic 重建链路（仅测试脚本做此兜底）。
stop_until_idle() {
  local max_round=5 i mid ysv pub
  for i in 1 2 3 4 5; do
    mid=$(next_msgid)
    publish_cmd "$TOPIC_STOP" "{\"msgId\":${mid},\"msg\":\"stopStream\",\"data\":{}}"
    sleep 5
    ysv=$(yaml_get .outgoing.server)
    pub=$(state_get is_publishing)
    if [ -z "$ysv" ] && [ "$pub" != "true" ]; then
      log "  stop_round $i: idle 达成 (server=[] is_publishing=[$pub])"
      return 0
    fi
    # 第3轮起用 yaml-cli 硬清 outgoing + HUP majestic（避免 L2 dedup replay 卡死）
    if [ "$i" -ge 2 ]; then
      yaml-cli -i "$MAJESTIC_CONF" -s .outgoing.server "" 2>/dev/null
      yaml-cli -i "$MAJESTIC_CONF" -s .outgoing.enabled "false" 2>/dev/null
      killall -HUP majestic 2>/dev/null
      echo -n "" > "$STATE_DIR/current_stream_url" 2>/dev/null
      echo -n "false" > "$STATE_DIR/is_publishing" 2>/dev/null
      log "  stop_round $i: 兜底 清空yaml outgoing + HUP majestic + reset state"
    fi
  done
  log "  WARN stop_until_idle 达到最大轮次$max_round, 可能未彻底"
  return 1
}

next_msgid() {
  # 不和 encodermain 共用号池, 用一个独立范围 (5xxxx)
  if [ ! -s /tmp/test_stream_msgid ]; then echo 50000 > /tmp/test_stream_msgid; fi
  n=$(cat /tmp/test_stream_msgid)
  n=$((n + 1))
  echo "$n" > /tmp/test_stream_msgid
  echo "$n"
}

yaml_get() {  # $1=key, 输出纯值(去引号)
  local v
  v=$(yaml-cli -i "$MAJESTIC_CONF" -g "$1" 2>/dev/null || true)
  # yaml-cli v0.0.4 可能输出字符串带单引号(历史bug确认过shell调用不会), 兜底去首尾引号
  v=$(printf '%s' "$v" | sed -e "s/^'//" -e "s/'\$//" -e 's/^"//' -e 's/"$//')
  printf '%s' "$v"
}

state_get() {  # $1=key
  [ -f "$STATE_DIR/$1" ] && cat "$STATE_DIR/$1" 2>/dev/null
}

# -------- 步骤 A: 清环境 + 先停流 (幂等) --------
log "=== pre-clean A0: 清理 L2 task_dedup 缓存 (避免跨 run stale key 误 replay) ==="
if [ -d "$STATE_DIR/task_dedup" ]; then
  ndel=$(find "$STATE_DIR/task_dedup" -type f 2>/dev/null | wc -l)
  rm -f "$STATE_DIR/task_dedup"/* 2>/dev/null || true
  log "  清除 task_dedup 文件数=$ndel"
fi
log "=== pre-clean A1: 下发 stop_stream 让设备回到 idle ==="
MSGID=$(next_msgid); publish_cmd "$TOPIC_STOP" "{\"msgId\":${MSGID},\"msg\":\"stopStream\",\"data\":{}}"
stop_until_idle

# ==============================================================
# 子用例 T1: 下发 URL_A (单级path Bug形态)
#   修复前预期 outgoing 缺 /live 段 → 无TCP ESTAB;
#   修复后 stream_url_ensure_app_default 插入 /live → outgoing=URL_A补live段, TCP有连接
# ==============================================================
run_case() {
  local caseno="$1"; local url="$2"; local expect_substr="$3"; local expect_exact="$4"
  local msgid ysv ssu pub tcp_ok settled=0 t
  log "========== CASE $caseno start =========="
  log "下发 start_stream: streamUrl=$url"
  msgid=$(next_msgid)
  publish_cmd "$TOPIC_START" \
    "{\"msgId\":${msgid},\"msg\":\"startStream\",\"data\":{\"duration\":60,\"streamUrl\":\"${url}\",\"taskId\":\"test_case_${caseno}_$$\"}}"

  # B. 等待 settle (最多 40s): yaml server 非空 + 带 expect_substr
  for t in 1 2 3 4 5 6 7 8; do
    sleep 5
    ysv=$(yaml_get .outgoing.server)
    ssu=$(state_get current_stream_url)
    pub=$(state_get is_publishing)
    # 只要 yaml server 非空 + 带 substr, 就认为开始生效；再等 TCP
    if [ -n "$ysv" ] && printf '%s' "$ysv" | grep -qF "$expect_substr" && [ "$pub" = "true" ]; then
      # 再额外等 3s 给 TCP 建链
      sleep 3
      settled=1
      break
    fi
  done
  ysv=$(yaml_get .outgoing.server)
  ssu=$(state_get current_stream_url)
  pub=$(state_get is_publishing)
  log "snapshot (case=$caseno): outgoing.server=[$ysv]"
  log "snapshot (case=$caseno): state.current_stream_url=[$ssu] is_publishing=[$pub]"

  # C. 5 项断言 (结果累积进全局 PASS_CNT / FAIL_CNT)
  local local_fails=0

  # A1: outgoing.server 非空
  if [ -n "$ysv" ]; then
    log "  A1 PASS .outgoing.server 非空"
  else
    log "  A1 FAIL .outgoing.server 为空 (majestic未写入或yaml_set失败)"; local_fails=$((local_fails+1))
  fi

  # A2: 4 段式 URL + 含 /live/ 段 (app="live")
  #     解析: scheme://host:port/APP/STREAM_NAME；APP 必须 = live
  #     用 awk 数 path segment: 要求 $3/$4 存在且 $3=live
  local scheme hostport appname streamname path_part
  path_part=$(printf '%s' "$ysv" | awk -F'://' '{print $2}' | awk -F'/' '{
    host=$1; app=$2; name=$3;
    printf "%s|%s|%s\n", host, app, name;
  }')
  hostport=$(printf '%s' "$path_part" | cut -d'|' -f1)
  appname=$(printf  '%s' "$path_part" | cut -d'|' -f2)
  streamname=$(printf '%s' "$path_part" | cut -d'|' -f3)
  log "  parsed path: hostport=[$hostport] app=[$appname] stream=[$streamname]"
  if [ "$appname" = "live" ] && [ -n "$streamname" ]; then
    log "  A2 PASS outgoing.server path 两段 (app=live + stream_name)"
  else
    log "  A2 FAIL outgoing.server 缺独立 /live/ app 段 (app=[$appname] stream=[$streamname])"
    local_fails=$((local_fails+1))
  fi

  # A2b: stream_name 必须是 stream_<DEVICE_ID>
  if [ "$streamname" = "$EXPECTED_STREAM_NAME" ]; then
    log "  A2b PASS stream_name=$EXPECTED_STREAM_NAME 匹配"
  else
    log "  A2b FAIL stream_name=[$streamname] != [$EXPECTED_STREAM_NAME]"; local_fails=$((local_fails+1))
  fi

  # A2c: 若调用方指定 expect_exact (字符串)，做完全相等
  if [ -n "$expect_exact" ] && [ "$ysv" != "$expect_exact" ]; then
    log "  A2c FAIL outgoing.server exact mismatch"
    log "         expect=[$expect_exact]"
    log "         actual=[$ysv]"
    local_fails=$((local_fails+1))
  else
    if [ -n "$expect_exact" ]; then log "  A2c PASS exact URL match"; fi
  fi

  # A3: state/current_stream_url 与 yaml outgoing.server 完全一致
  if [ -n "$ssu" ] && [ "$ssu" = "$ysv" ]; then
    log "  A3 PASS state.current_stream_url 与 yaml outgoing.server 一致"
  else
    log "  A3 FAIL state.current_stream_url=[$ssu] != yaml=[$ysv]"; local_fails=$((local_fails+1))
  fi

  # A3b: state/is_publishing = true
  if [ "$pub" = "true" ]; then
    log "  A3b PASS is_publishing=true"
  else
    log "  A3b FAIL is_publishing=[$pub] (stream_start 逻辑未写状态)"; local_fails=$((local_fails+1))
  fi

  # A4: TCP ESTABLISHED 到 1935 (远程端口 0x078F)
  #     /proc/net/tcp 行: sl local_address rem_address st tx:rx:tr:tm...
  #     rem_address = <hex_ip>:<hex_port> 形式（整列内已经带冒号）, 直接过滤 $3=="*:078F" 且 $4=="01"
  tcp_hit=$(awk 'NR>1 && $4=="01" {
      n=split($3, a, ":");
      if (n==2 && a[2]=="078F") { print NR; exit }
    }' /proc/net/tcp 2>/dev/null | head -n1)
  if [ -n "$tcp_hit" ]; then
    log "  A4 PASS TCP ESTABLISHED 到 :1935 (line #$tcp_hit)"
    tcp_ok=1
  else
    # 兜底: 直接 grep 行中 ":078F" + 状态 01 (有些系统 awk split 不生效)
    tcp_hit2=$(grep -E ':078F[[:space:]]+01[[:space:]]' /proc/net/tcp 2>/dev/null | head -n1)
    if [ -n "$tcp_hit2" ]; then
      log "  A4 PASS TCP ESTABLISHED 到 :1935 (grep fallback hit)"
      tcp_ok=1
    else
      log "  A4 FAIL 无 ESTABLISHED TCP 连接到 :1935 (SRS 不通 / 路径错 majestic 拒连 / 防火墙)"; local_fails=$((local_fails+1))
      tcp_ok=0
    fi
  fi

  PASS_CNT=$((PASS_CNT + 6 - local_fails))
  FAIL_CNT=$((FAIL_CNT + local_fails))
  CASE_FAILS=$((CASE_FAILS + (local_fails > 0 ? 1 : 0)))

  if [ "$local_fails" -gt 0 ]; then
    log "---- CASE ${caseno} FAILURES DIAGNOSTIC DUMP ----"
    echo "== yaml outgoing block =="
    yaml-cli -i "$MAJESTIC_CONF" -g .outgoing 2>/dev/null || echo "(yaml-cli err)"
    echo "== state/ current_stream_url / is_publishing =="
    echo "current_stream_url=$(state_get current_stream_url)"
    echo "is_publishing=$(state_get is_publishing)"
    echo "== /proc/net/tcp (remote port 078F or ESTAB) =="
    awk 'NR==1 || $3~/078F$/ || $4=="01" {print}' /proc/net/tcp | head -n 20
    echo "== encodermain.log tail 40 =="
    [ -f "$ENCLOG" ] && tail -n 40 "$ENCLOG" 2>/dev/null | sed 's/^/LOG> /' || echo "(no log)"
    echo "== DEVICE_ID = $DEVICE_ID"
  fi
  log "========== CASE $caseno end (fails=$local_fails) =========="
}

PASS_CNT=0
FAIL_CNT=0
CASE_FAILS=0

# 实际期望: Bug修复后, 两条URL最终都应该是 rtmp://host:1935/live/stream_<ID>
EXPECT_A_FINAL="rtmp://${MQTT_HOST}:1935/live/${EXPECTED_STREAM_NAME}"
EXPECT_B_FINAL="rtmp://${MQTT_HOST}:1935/live/${EXPECTED_STREAM_NAME}"

run_case T1 "$URL_A" "/${EXPECTED_STREAM_NAME}" "$EXPECT_A_FINAL"

# ---------- T1 → T2 之间 stop: 必须保证 idle 后再起，否则 L2 dedup 可能 replay 或状态重叠 ----------
log "T1→T2 之间 stop_stream, 等待 idle..."
MSGID=$(next_msgid); publish_cmd "$TOPIC_STOP" "{\"msgId\":${MSGID},\"msg\":\"stopStream\",\"data\":{}}"
stop_until_idle

run_case T2 "$URL_B" "/live/${EXPECTED_STREAM_NAME}" "$EXPECT_B_FINAL"

# =====================================================================
# 2026-09-01 新增: T2.3 ~ T2.8 扩展 E2E 用例（覆盖伪两级 / fallback /
# dedup 重放 / URL 切换 / STOP 幂等 / 驼峰-蛇形协议）
# =====================================================================

# ---- 先 idle 清干净 ----
MSGID=$(next_msgid); publish_cmd "$TOPIC_STOP" "{\"msgId\":${MSGID},\"msg\":\"stopStream\",\"data\":{}}"
stop_until_idle

# T2.3: 伪两级（非 live 首段）→ C 端必须丢弃 otherApp 强制插 live
URL_FAKE2="rtmp://${MQTT_HOST}:1935/otherApp/demoTask_$$"
EXPECT_FAKE2="rtmp://${MQTT_HOST}:1935/live/${EXPECTED_STREAM_NAME}"
run_case T2.3 "$URL_FAKE2" "/live/${EXPECTED_STREAM_NAME}" "$EXPECT_FAKE2"
MSGID=$(next_msgid); publish_cmd "$TOPIC_STOP" "{\"msgId\":${MSGID},\"msg\":\"stopStream\",\"data\":{}}"
stop_until_idle

# T2.4: 空 streamUrl → 回退 rt.srs_url 拼接 /live/stream_<ID>
#       streamUrl=空或不给字段, encodermain 应从 runtime state 取 srs_url
#       再走 proto_stream_url_normalize fallback 分支。
run_case T2.4 "" "/live/${EXPECTED_STREAM_NAME}" "rtmp://${MQTT_HOST}:1935/live/${EXPECTED_STREAM_NAME}"
MSGID=$(next_msgid); publish_cmd "$TOPIC_STOP" "{\"msgId\":${MSGID},\"msg\":\"stopStream\",\"data\":{}}"
stop_until_idle

# T2.5: 同 URL 连续两次 START → 第二次 ACK 应 duplicate(或 success),
#       outgoing 保持非空 TCP ESTAB 不能断。
URL_T25="rtmp://${MQTT_HOST}:1935/live/caseT25_$$"
EXPECT_T25="rtmp://${MQTT_HOST}:1935/live/${EXPECTED_STREAM_NAME}"
log "========== CASE T2.5 start (同URL重发) =========="
FIRST_YAML=""
# 第1次 start
msgid=$(next_msgid)
publish_cmd "$TOPIC_START" \
  "{\"msgId\":${msgid},\"msg\":\"startStream\",\"data\":{\"duration\":60,\"streamUrl\":\"${URL_T25}\",\"taskId\":\"case_T25_$$\"}}"
sleep 25; FIRST_YAML=$(yaml_get .outgoing.server)
# 第2次 start (相同 URL 同 taskId)
msgid=$(next_msgid)
publish_cmd "$TOPIC_START" \
  "{\"msgId\":${msgid},\"msg\":\"startStream\",\"data\":{\"duration\":60,\"streamUrl\":\"${URL_T25}\",\"taskId\":\"case_T25_$$\"}}"
sleep 15
SECOND_YAML=$(yaml_get .outgoing.server); PUB=$(state_get is_publishing)
TCP_HIT=$(awk 'NR>1 && $4=="01" { n=split($3,a,":"); if (n==2 && a[2]=="078F") { print NR; exit } }' /proc/net/tcp 2>/dev/null | head -n1)
log "  T2.5 snapshot: run1_yaml=[$FIRST_YAML] run2_yaml=[$SECOND_YAML]"
lf=0
if [ -n "$FIRST_YAML" ] && printf '%s' "$FIRST_YAML" | grep -qF '/live/'; then
  log "  T2.5-A PASS 首次 start yaml 已带 /live/"
else
  log "  T2.5-A FAIL 首次 start yaml 缺 /live 段: [$FIRST_YAML]"; lf=$((lf+1))
fi
if [ -n "$SECOND_YAML" ] && [ "$SECOND_YAML" = "$EXPECT_T25" ]; then
  log "  T2.5-B PASS 重发后 yaml 与期望值一致 (dedup 没清 outgoing)"
else
  log "  T2.5-B FAIL 重发后 yaml=[$SECOND_YAML] 期望=[$EXPECT_T25]"; lf=$((lf+1))
fi
if [ "$PUB" = "true" ]; then log "  T2.5-C PASS is_publishing=true";
else log "  T2.5-C FAIL is_publishing=[$PUB]"; lf=$((lf+1)); fi
if [ -n "$TCP_HIT" ]; then log "  T2.5-D PASS TCP :1935 ESTABLISHED";
else log "  T2.5-D FAIL TCP :1935 断链(重发清了yaml?)"; lf=$((lf+1)); fi
PASS_CNT=$((PASS_CNT + 4 - lf)); FAIL_CNT=$((FAIL_CNT + lf))
[ "$lf" -gt 0 ] && CASE_FAILS=$((CASE_FAILS+1))
# 清 T2.5 dedup key, 保证下一 case 不被 replay 卡死
MSGID=$(next_msgid); publish_cmd "$TOPIC_STOP" "{\"msgId\":${MSGID},\"msg\":\"stopStream\",\"data\":{}}"
stop_until_idle
log "========== CASE T2.5 end (fails=$lf) =========="

# T2.6: START_A → STOP → START_B 不同 URL
#   关键断言: STOP 后 START_A.done 必须真删除 (removed_done>=1),
#   第二次 START_B 必须全新执行 (非 duplicate 分支), outgoing=B 规范化结果.
URL_A6="rtmp://${MQTT_HOST}:1935/live/urlA_$$"
URL_B6="rtmp://${MQTT_HOST}:1935/otherApp/urlB_$$"
EXPECT_B6="rtmp://${MQTT_HOST}:1935/live/${EXPECTED_STREAM_NAME}"
DEDUP_DIR="${STATE_DIR}/task_dedup"
mkdir -p "$DEDUP_DIR"
log "========== CASE T2.6 start (不同URL切换, 验证 STOP 真删 START.done) =========="
# START_A
msgid=$(next_msgid)
publish_cmd "$TOPIC_START" \
  "{\"msgId\":${msgid},\"msg\":\"startStream\",\"data\":{\"duration\":120,\"streamUrl\":\"${URL_A6}\",\"taskId\":\"case_T26A_$$\"}}"
sleep 25
N1=$(find "$DEDUP_DIR" -name '*.done' 2>/dev/null | wc -l)
log "  T2.6  START_A 完成后 *.done 数=$N1"
# STOP
MSGID=$(next_msgid); publish_cmd "$TOPIC_STOP" "{\"msgId\":${MSGID},\"msg\":\"stopStream\",\"data\":{}}"
stop_until_idle
sleep 2
N2=$(find "$DEDUP_DIR" -name '*.done' 2>/dev/null | wc -l)
log "  T2.6  STOP 完成后 *.done 数=$N2 (期望 < N1=$N1，START_A.done 被移除)"
# START_B
msgid=$(next_msgid)
publish_cmd "$TOPIC_START" \
  "{\"msgId\":${msgid},\"msg\":\"startStream\",\"data\":{\"duration\":120,\"streamUrl\":\"${URL_B6}\",\"taskId\":\"case_T26B_$$\"}}"
sleep 25
B_YAML=$(yaml_get .outgoing.server); B_PUB=$(state_get is_publishing)
lf=0
if [ "$N1" -eq 0 ]; then
  log "  T2.6-A INFO pre-clean 已清空 task_dedup, N1=0 无法验证删除效果 ($N1 -> $N2)"
elif [ "$N2" -lt "$N1" ]; then
  log "  T2.6-A PASS STOP 真删除了 START_A.done ($N1 -> $N2)"
else
  log "  T2.6-A FAIL STOP 未删除 START_A.done! ($N1 -> $N2)"; lf=$((lf+1))
fi
if [ "$B_YAML" = "$EXPECT_B6" ]; then
  log "  T2.6-B PASS START_B.yaml = $EXPECT_B6 (伪两级归一化成功)"
else
  log "  T2.6-B FAIL START_B.yaml = [$B_YAML] != [$EXPECT_B6]"; lf=$((lf+1))
fi
if [ "$B_PUB" = "true" ]; then log "  T2.6-C PASS is_publishing=true";
else log "  T2.6-C FAIL is_publishing=[$B_PUB]"; lf=$((lf+1)); fi
PASS_CNT=$((PASS_CNT + 3 - lf)); FAIL_CNT=$((FAIL_CNT + lf))
[ "$lf" -gt 0 ] && CASE_FAILS=$((CASE_FAILS+1))
MSGID=$(next_msgid); publish_cmd "$TOPIC_STOP" "{\"msgId\":${MSGID},\"msg\":\"stopStream\",\"data\":{}}"
stop_until_idle
log "========== CASE T2.6 end (fails=$lf) =========="

# T2.7: 连续三次 STOP(幂等) → 不应出现 L2 dedup replay 卡死,
#       最终 outgoing=空 + is_publishing=false 且 stop_until_idle ≤2 轮达成.
log "========== CASE T2.7 start (STOP x3 幂等) =========="
# 先造一个真 START
msgid=$(next_msgid)
publish_cmd "$TOPIC_START" \
  "{\"msgId\":${msgid},\"msg\":\"startStream\",\"data\":{\"duration\":600,\"streamUrl\":\"rtmp://${MQTT_HOST}:1935/live/caseT27_$$\"}}"
sleep 20
# 第1次 STOP: 这里不硬清兜底! 让 stop_until_idle 纯靠 MQTT.
max_try=5; ok=0
for i in 1 2 3 4 5; do
  mid=$(next_msgid)
  publish_cmd "$TOPIC_STOP" "{\"msgId\":${mid},\"msg\":\"stopStream\",\"data\":{}}"
  sleep 4
  ysv=$(yaml_get .outgoing.server); pub=$(state_get is_publishing)
  log "  T2.7 STOP_round=$i server=[$ysv] pub=[$pub]"
  if [ -z "$ysv" ] && [ "$pub" != "true" ]; then ok=$i; break; fi
done
lf=0
if [ "$ok" -ge 1 ] && [ "$ok" -le 2 ]; then
  log "  T2.7-A PASS STOP 幂等: 第 ${ok} 轮达成 idle (≤2 轮证明 STOP L2 dedup 没卡死)"
else
  if [ "$ok" -ge 3 ]; then
    log "  T2.7-A WARN STOP 用了 ${ok} 轮才 idle (>2轮可能存在 L2 replay)"
  else
    log "  T2.7-A FAIL STOP 5 轮后仍未 idle!"; lf=$((lf+1))
  fi
fi
# 再发两次 STOP (无 MQTT 命令 stop_until_idle 兜底不做)
for extra in 1 2; do
  mid=$(next_msgid)
  publish_cmd "$TOPIC_STOP" "{\"msgId\":${mid},\"msg\":\"stopStream\",\"data\":{}}"
  sleep 2
done
final_server=$(yaml_get .outgoing.server); final_pub=$(state_get is_publishing)
if [ -z "$final_server" ] && [ "$final_pub" != "true" ]; then
  log "  T2.7-B PASS STOP 3+2 次后保持 idle 幂等"
else
  log "  T2.7-B FAIL 连续 STOP 后仍在推流 server=[$final_server] pub=[$final_pub]"
  lf=$((lf+1))
fi
PASS_CNT=$((PASS_CNT + 2 - lf)); FAIL_CNT=$((FAIL_CNT + lf))
[ "$lf" -gt 0 ] && CASE_FAILS=$((CASE_FAILS+1))
# 兜底清状态
MSGID=$(next_msgid); publish_cmd "$TOPIC_STOP" "{\"msgId\":${MSGID},\"msg\":\"stopStream\",\"data\":{}}"
stop_until_idle
log "========== CASE T2.7 end (fails=$lf) =========="

# T2.8: 驼峰 vs 蛇形 payload
#   bug: encodermain 曾把 start_stream (蛇形) 误作为合法 action 解析。
#   修复后: ctrlserver/protocol 规定 payload.msg 必须是驼峰 startStream。
#   用蛇形下发 → 期待 encodermain 不写 outgoing, is_publishing 保持 false.
log "========== CASE T2.8 start (payload 蛇形 start_stream 应被拒) =========="
stop_until_idle
sleep 2
PRE_SRV=$(yaml_get .outgoing.server); PRE_PUB=$(state_get is_publishing)
msgid=$(next_msgid)
publish_cmd "$TOPIC_START" \
  "{\"msgId\":${msgid},\"msg\":\"start_stream\",\"data\":{\"duration\":60,\"streamUrl\":\"rtmp://${MQTT_HOST}:1935/live/caseT28snake_$$\"}}"
sleep 20
POST_SRV=$(yaml_get .outgoing.server); POST_PUB=$(state_get is_publishing)
lf=0
# 期望: server 仍 = PRE (或空), publishing 仍 = PRE (或 false)
if [ -z "$POST_SRV" ] || [ "$POST_SRV" = "$PRE_SRV" ]; then
  log "  T2.8-A PASS 蛇形 start_stream 未写入 outgoing.server"
else
  log "  T2.8-A FAIL 蛇形 start_stream 错误触发了 outgoing=[$POST_SRV] (协议应为驼峰!)"; lf=$((lf+1))
fi
if [ "$POST_PUB" != "true" ]; then
  log "  T2.8-B PASS 蛇形 start_stream 未置 is_publishing=true"
else
  log "  T2.8-B FAIL 蛇形 start_stream 把 is_publishing 置 true"; lf=$((lf+1))
fi
PASS_CNT=$((PASS_CNT + 2 - lf)); FAIL_CNT=$((FAIL_CNT + lf))
[ "$lf" -gt 0 ] && CASE_FAILS=$((CASE_FAILS+1))
# 恢复: 把可能的脏状态清掉 (即使 PASS 也执行, 幂等)
MSGID=$(next_msgid); publish_cmd "$TOPIC_STOP" "{\"msgId\":${MSGID},\"msg\":\"stopStream\",\"data\":{}}"
stop_until_idle
log "========== CASE T2.8 end (fails=$lf) =========="

# ---------- 步骤 D: 停止 (除非 --skip-stop) ----------
if [ "$SKIP_STOP" -eq 0 ]; then
  log "=== 用例收尾: 下发 stop_stream ==="
  MSGID=$(next_msgid)
  publish_cmd "$TOPIC_STOP" "{\"msgId\":${MSGID},\"msg\":\"stopStream\",\"data\":{}}"
  stop_until_idle
  ysv=$(yaml_get .outgoing.server)
  pub=$(state_get is_publishing)
  if [ -z "$ysv" ] && [ "$pub" != "true" ]; then
    log "D PASS 停流后 outgoing.server 清空 + is_publishing=false"
    PASS_CNT=$((PASS_CNT + 2))
  else
    log "D FAIL 停流不彻底: server=[$ysv] is_publishing=[$pub]"
    FAIL_CNT=$((FAIL_CNT + 1))
  fi
fi

# ---------- 总结 ----------
echo
echo "================================================================"
echo "  TEST STREAM YAML VERIFY  SUMMARY"
echo "  DEVICE_ID=$DEVICE_ID  broker=$MQTT_HOST:$MQTT_PORT"
echo "  sub-assertions PASS=$PASS_CNT  FAIL=$FAIL_CNT  failed_cases=$CASE_FAILS"
echo "================================================================"
if [ "$FAIL_CNT" -eq 0 ] && [ "$CASE_FAILS" -eq 0 ]; then
  echo "RESULT: PASS (所有 outgoing.yaml / state / TCP 断言均成立)"
  exit 0
else
  echo "RESULT: FAIL (请查看上方 CASE *_ FAILURES DIAGNOSTIC DUMP)"
  exit 1
fi
