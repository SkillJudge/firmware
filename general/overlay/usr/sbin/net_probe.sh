#!/bin/sh
# ======================================================================
# net_probe.sh — 编码器 WiFi 网络质量测试 (产线)
#
# 用法:
#   sh net_probe.sh [ping_target] [ftp_server_ip]
#     ping_target    : ping 目标 IP (默认 192.168.250.100, 有线 Pi)
#     ftp_server_ip  : 产线工具 FTP 服务器 IP (可选, 用于上传吞吐测试)
#
# 测试项:
#   1. WiFi 信号强度 RSSI (dBm)         <- /proc/net/wireless
#   2. 丢包率 / 平均延迟 / 抖动         <- ping 有线 Pi
#   3. FTP 上传吞吐 (Mbps)              <- curl 上传 1MB 到产线 FTP (可选)
#
# 输出: 行式报告 NETCHK name=value ev=[evidence]
#       NETSUMMARY ...
#       NETGRADE=好|良好|一般|很差
#
# 判定标准 (取所有指标中最差的等级作为最终结论):
#   指标          好            良好          一般          很差
#   RSSI(dBm)    >= -55        -55 ~ -65     -65 ~ -75     < -75
#   丢包率        0%            0 ~ 1%        1 ~ 5%        > 5%
#   平均延迟      < 5ms         5 ~ 15ms      15 ~ 50ms     > 50ms
#   抖动(max-min) < 3ms         3 ~ 10ms      10 ~ 30ms     > 30ms
#   上传吞吐*     > 10Mbps      5 ~ 10Mbps    2 ~ 5Mbps     < 2Mbps
#   * 吞吐项仅在 FTP 上传成功时参与判定, 失败不降级
# ======================================================================
set -u

PING_TARGET="${1:-192.168.250.100}"
FTP_IP="${2:-}"
PING_COUNT=50
PING_WAIT=2

# 等级: 0=好 1=良好 2=一般 3=很差
GRADE=0
grade_worse() {
  # $1 = 新等级数值; 若比当前 GRADE 差则更新
  [ "$1" -gt "$GRADE" ] && GRADE="$1"
}

grade_name() {
  case "$1" in
    0) echo "好" ;;
    1) echo "良好" ;;
    2) echo "一般" ;;
    *) echo "很差" ;;
  esac
}

echo "NETINFO net_probe v1 target=$PING_TARGET ftp_ip=${FTP_IP:-none} date=$(date '+%F %T')"

# ======================================================================
# 1. WiFi 信号强度 RSSI (dBm)
# ======================================================================
RSSI=$(awk 'NR>2{gsub(/\./,"",$4); print $4}' /proc/net/wireless 2>/dev/null | head -1)
if [ -z "$RSSI" ] || [ "$RSSI" = "null" ]; then
  # 回退 iwconfig
  RSSI=$(iwconfig wlan0 2>/dev/null | grep -o 'Signal level=[-0-9]*' | cut -d= -f2)
fi
if [ -n "$RSSI" ] && [ "$RSSI" -lt 0 ] 2>/dev/null; then
  echo "NETCHK rssi=${RSSI}dBm ev=[/proc/net/wireless]"
  if   [ "$RSSI" -ge -55 ]; then grade_worse 0
  elif [ "$RSSI" -ge -65 ]; then grade_worse 1
  elif [ "$RSSI" -ge -75 ]; then grade_worse 2
  else grade_worse 3; fi
else
  echo "NETCHK rssi=N/A ev=[无法读取 RSSI]"
  grade_worse 3
fi

# ======================================================================
# 2. ping 有线目标: 丢包率 / 延迟 / 抖动
# ======================================================================
PING_OUT=$(ping -c "$PING_COUNT" -W "$PING_WAIT" "$PING_TARGET" 2>&1)
PING_RC=$?

# 丢包率: "50 packets transmitted, 48 received, 4% packet loss"
LOSS=$(echo "$PING_OUT" | grep -oE '[0-9]+% packet loss' | grep -oE '[0-9]+')
[ -z "$LOSS" ] && LOSS=100

# 延迟: "round-trip min/avg/max = 1.2/2.3/4.5 ms" (部分 busybox 无 round-trip 前缀)
RTT_LINE=$(echo "$PING_OUT" | grep 'min/avg/max')
# 取等号后内容, 按 '/' 切三段, 去掉尾部 " ms"
RTT_VALS=$(echo "$RTT_LINE" | sed 's/.*= *//')
MIN_MS=$(echo "$RTT_VALS" | cut -d/ -f1)
AVG_MS=$(echo "$RTT_VALS" | cut -d/ -f2)
MAX_MS=$(echo "$RTT_VALS" | cut -d/ -f3 | sed 's/ .*//')

# 抖动 = max - min (busybox awk 浮点)
JITTER=""
if [ -n "$MIN_MS" ] && [ -n "$MAX_MS" ]; then
  JITTER=$(awk -v a="$MAX_MS" -v b="$MIN_MS" 'BEGIN{printf "%.2f", a-b}')
fi

echo "NETCHK ping_loss=${LOSS}% ev=[${PING_COUNT} packets, rc=$PING_RC]"
# 丢包率等级
if   [ "$LOSS" -eq 0 ]; then grade_worse 0
elif [ "$LOSS" -le 1 ]; then grade_worse 1
elif [ "$LOSS" -le 5 ]; then grade_worse 2
else grade_worse 3; fi

if [ -n "$AVG_MS" ]; then
  echo "NETCHK ping_avg=${AVG_MS}ms ev=[min=${MIN_MS} max=${MAX_MS}]"
  # 平均延迟等级 (浮点比较用 awk)
  G_AVG=$(awk -v a="$AVG_MS" 'BEGIN{ if(a<5) print 0; else if(a<15) print 1; else if(a<50) print 2; else print 3 }')
  grade_worse "$G_AVG"
else
  echo "NETCHK ping_avg=N/A ev=[无 RTT 统计]"
  grade_worse 3
fi

if [ -n "$JITTER" ]; then
  echo "NETCHK ping_jitter=${JITTER}ms ev=[max-min]"
  G_JIT=$(awk -v a="$JITTER" 'BEGIN{ if(a<3) print 0; else if(a<10) print 1; else if(a<30) print 2; else print 3 }')
  grade_worse "$G_JIT"
else
  echo "NETCHK ping_jitter=N/A ev=[无法计算]"
fi

# ======================================================================
# 3. FTP 上传吞吐 (可选, 需 ftp_server_ip)
# ======================================================================
if [ -n "$FTP_IP" ]; then
  TEST_FILE=/tmp/netprobe_1mb.bin
  # 生成 1MB 测试数据
  dd if=/dev/zero of="$TEST_FILE" bs=1024 count=1024 2>/dev/null
  if [ -f "$TEST_FILE" ]; then
    T0=$(awk '{print int($1)}' /proc/uptime)
    curl -s -T "$TEST_FILE" "ftp://${FTP_IP}:2121/netprobe_upload.bin" --connect-timeout 5 --max-time 30 >/dev/null 2>&1
    CURL_RC=$?
    T1=$(awk '{print int($1)}' /proc/uptime)
    DUR=$((T1 - T0))
    rm -f "$TEST_FILE"
    if [ "$CURL_RC" -eq 0 ] && [ "$DUR" -gt 0 ]; then
      # 吞吐 Mbps = (1MB * 8) / DUR秒
      THROUGH=$(awk -v d="$DUR" 'BEGIN{printf "%.2f", (1024*1024*8)/d/1000000}')
      echo "NETCHK ftp_upload=${THROUGH}Mbps ev=[1MB in ${DUR}s]"
      G_TP=$(awk -v a="$THROUGH" 'BEGIN{ if(a>10) print 0; else if(a>=5) print 1; else if(a>=2) print 2; else print 3 }')
      grade_worse "$G_TP"
    elif [ "$CURL_RC" -eq 0 ] && [ "$DUR" -eq 0 ]; then
      echo "NETCHK ftp_upload=>30Mbps ev=[1MB in <1s]"
      grade_worse 0
    else
      echo "NETCHK ftp_upload=N/A ev=[curl rc=$CURL_RC, 上传失败, 不参与判定]"
    fi
  else
    echo "NETCHK ftp_upload=N/A ev=[测试文件生成失败]"
  fi
else
  echo "NETCHK ftp_upload=N/A ev=[未指定 FTP 服务器 IP]"
fi

# ======================================================================
# 汇总
# ======================================================================
echo "NETSUMMARY rssi=${RSSI:-N/A} loss=${LOSS}% avg=${AVG_MS:-N/A}ms jitter=${JITTER:-N/A}ms"
echo "NETGRADE=$(grade_name $GRADE)"
exit "$GRADE"
