#!/bin/sh
# ======================================================================
# wifi_link_test.sh — 编码器 WiFi 连接详细测试（产线/调试用）
#
# 适用: OpenIPC GK7205V300 + E103-RTL8811CU(8821cu 驱动)，busybox ash
#
# 用法:
#   sh wifi_link_test.sh                # 完整测试，ping 网关，RSSI 采样 10 次
#   sh wifi_link_test.sh <ping目标>     # 指定 ping 目标，如 192.168.250.100
#   sh wifi_link_test.sh <ping目标> 30  # 指定 ping 目标，RSSI 采样 30 次
#   sh wifi_link_test.sh scan           # 测试后附加扫描周边 AP（会短暂跳信道）
#
# 输出: 控制台直接打印，可用 > report.txt 落盘
#
# 判定标准（与 net_probe.sh 一致）:
#   RSSI >= -55dBm 好 / -55~-65 良好 / -65~-75 一般 / < -75 很差
# ======================================================================
set -u

IFACE=wlan0
PING_TARGET="${1:-}"
[ "$PING_TARGET" = "scan" ] && PING_TARGET=""
DO_SCAN=0
for a in "$@"; do [ "$a" = "scan" ] && DO_SCAN=1; done
SAMPLES="${2:-10}"
[ "$SAMPLES" = "scan" ] && SAMPLES=10
PING_COUNT=20
PING_WAIT=2

hr() { echo "----------------------------------------------------------------------"; }
sec() { echo ""; echo "===== $1 ====="; }

have() { command -v "$1" >/dev/null 2>&1; }

# 频率(MHz) -> 频段
band_of() { awk -v f="$1" 'BEGIN{
  if (f>=2400 && f<=2500) print "2.4G";
  else if (f>=4900 && f<=5925) print "5G";
  else if (f>0) print "未知(" f "MHz)";
  else print "未知";
}'; }

# 频率(MHz) -> 信道号
chan_of() { awk -v f="$1" 'BEGIN{
  if (f==2484) { print 14; exit }
  if (f>=2412 && f<=2472) { print int((f-2407)/5); exit }
  if (f>=5000) {
    n=split("36 40 44 48 52 56 60 64 100 104 108 112 116 120 124 128 132 136 140 144 149 153 157 161 165", cs, " ");
    m=split("5180 5200 5220 5240 5260 5280 5300 5320 5500 5520 5540 5560 5580 5600 5620 5640 5660 5680 5700 5720 5745 5765 5785 5805 5825", fs, " ");
    for (i=1;i<=m;i++) if (fs[i]==f) { print cs[i]; exit }
  }
  print "?";
}'; }

grade_of() { awk -v r="$1" 'BEGIN{
  if (r==0||r=="") { print "N/A"; exit }
  if (r>=-55) print "好"; else if (r>=-65) print "良好";
  else if (r>=-75) print "一般"; else print "很差";
}'; }

echo "########## WiFi 连接详细测试 ##########"
echo "时间: $(date 2>/dev/null)"
echo "主机: $(hostname 2>/dev/null)  内核: $(uname -sr 2>/dev/null)"

# ------------------------------------------------------------------
sec "1. 接口 / IP / 网关"
if [ ! -d /sys/class/net/$IFACE ]; then
  echo "!! 接口 $IFACE 不存在，驱动未加载？先执行: /etc/wireless/usb rtl8811cu-generic"
  echo "   现有接口: $(ls /sys/class/net 2>/dev/null | tr '\n' ' ')"
  exit 1
fi
ip addr show $IFACE 2>/dev/null | grep -E 'inet |link/' || ifconfig $IFACE 2>/dev/null
GW=$(ip route 2>/dev/null | awk '/default/ {print $3; exit}')
[ -z "$GW" ] && GW=$(route -n 2>/dev/null | awk '$1=="0.0.0.0"{print $2; exit}')
echo "默认网关: ${GW:-未获取到}"
[ -z "$PING_TARGET" ] && PING_TARGET="$GW"
echo "ping 目标: ${PING_TARGET:-无（跳过 ping）}"

# ------------------------------------------------------------------
sec "2. USB 模组枚举 / USB 协商速率（需 480M = USB2.0 High-Speed）"
USB_OK=0
for d in /sys/bus/usb/devices/*; do
  [ -f "$d/idVendor" ] || continue
  v=$(cat "$d/idVendor" 2>/dev/null); p=$(cat "$d/idProduct" 2>/dev/null)
  case "$v" in
    0bda|c821)
      spd=$(cat "$d/speed" 2>/dev/null)
      echo "设备: $v:$p  speed=${spd}M  $(cat "$d/product" 2>/dev/null)  ($d)"
      USB_OK=1
      if [ "$spd" = "480" ]; then echo "  -> USB2.0 High-Speed，正常"; else
        echo "  -> !! 速率异常，非 480M，吞吐会被 USB 总线卡死"; fi
      ;;
  esac
done
[ "$USB_OK" = "0" ] && echo "!! 未发现 Realtek(0bda) USB WiFi 设备"
have lsusb && lsusb | grep -iE '0bda|realtek'

# ------------------------------------------------------------------
sec "3. 驱动 / 模块"
lsmod 2>/dev/null | grep -iE '8821|8811|cfg80211' || echo "(lsmod 无匹配)"
if [ -d /sys/module/8821cu/parameters ]; then
  echo "--- 8821cu 驱动关键参数 ---"
  for k in rtw_power_mgnt rtw_ips_mode rtw_lps_level rtw_country_code rtw_channel_plan; do
    [ -f /sys/module/8821cu/parameters/$k ] && echo "  $k = $(cat /sys/module/8821cu/parameters/$k 2>/dev/null)"
  done
fi

# ------------------------------------------------------------------
sec "4. 关联信息（SSID / BSSID / 频段 / 信道 / RSSI / 速率 / 频宽）"

# 采集三路原始数据，后面逐项解析（iw 不可用时自动降级）
IW_LINK=""; IW_STA=""; IWC=""
have iw && {
  IW_LINK=$(iw dev $IFACE link 2>/dev/null)
  IW_STA=$(iw dev $IFACE station dump 2>/dev/null)
}
have iwconfig && IWC=$(iwconfig $IFACE 2>/dev/null)

if echo "$IW_LINK" | grep -q 'Connected to'; then
  : # iw 正常报告已关联
elif echo "$IWC" | grep -q 'Access Point: ' && \
     ! echo "$IWC" | grep -q 'Access Point: Not-Associated' && \
     ! echo "$IWC" | grep -q 'Access Point: 00:00:00:00:00:00'; then
  : # iw 不可用/驱动不支持，但 iwconfig 显示已关联 AP
else
  echo "!! $IFACE 当前未关联任何 AP"
  echo "--- iwconfig 原始输出 ---"; echo "$IWC" | head -8
  echo "!! 请先确认 wpa_supplicant 已连上 WiFi，再跑本脚本"
fi

SSID=$(echo "$IW_LINK" | awk -F': ' '/SSID/{print $2; exit}')
[ -z "$SSID" ] && SSID=$(echo "$IWC" | sed -n 's/.*ESSID:"\(.*\)".*/\1/p' | head -1)
BSSID=$(echo "$IW_LINK" | awk '/Connected to/{print $3; exit}')
[ -z "$BSSID" ] || [ "$BSSID" = "00:00:00:00:00:00" ] && \
  BSSID=$(echo "$IWC" | sed -n 's/.*Access Point: \(..:..:..:..:..:..\).*/\1/p' | head -1)

FREQ=$(echo "$IW_LINK" | awk '/freq:/{print $2; exit}')
if [ -z "$FREQ" ]; then
  FREQ_G=$(echo "$IWC" | grep -o 'Frequency:[0-9.]*' | cut -d: -f2 | head -1)
  [ -n "$FREQ_G" ] && FREQ=$(awk -v x="$FREQ_G" 'BEGIN{printf "%d", x*1000+0.5}')
fi

RSSI=$(echo "$IW_LINK" | awk '/signal:/{print $2; exit}')
[ -z "$RSSI" ] && RSSI=$(echo "$IWC" | grep -o 'Signal level=[-0-9]*' | cut -d= -f2 | head -1)
[ -z "$RSSI" ] && RSSI=$(awk 'NR>2{gsub(/\./,"",$4); print $4}' /proc/net/wireless 2>/dev/null | head -1)

BAND=$(band_of "${FREQ:-0}")
CHAN=$(chan_of "${FREQ:-0}")

# 收发速率 + 协议 + 频宽（优先 station dump，其次 link，最后 iwconfig）
RATE_LINE=$(echo "$IW_STA" | awk '/tx bitrate/{print; exit}')
[ -z "$RATE_LINE" ] && RATE_LINE=$(echo "$IW_LINK" | awk '/tx bitrate/{print; exit}')
RX_RATE_LINE=$(echo "$IW_STA" | awk '/rx bitrate/{print; exit}')
TX_RATE=$(echo "$RATE_LINE" | awk '{print $3}')
RX_RATE=$(echo "$RX_RATE_LINE" | awk '{print $3}')
WIDTH=$(echo "$IW_LINK
$IW_STA" | grep -oE 'width: [0-9]+ MHz|[0-9]+MHz' | grep -oE '[0-9]+' | sort -rn | head -1)
PROTO=""
echo "$RATE_LINE" | grep -q VHT && PROTO="802.11ac (VHT)"
[ -z "$PROTO" ] && echo "$RATE_LINE" | grep -q ' HE' && PROTO="802.11ax (HE)"
[ -z "$PROTO" ] && echo "$RATE_LINE" | grep -q ' HT' && PROTO="802.11n (HT)"
if [ -z "$TX_RATE" ]; then
  TX_RATE=$(echo "$IWC" | grep -o 'Bit Rate=[0-9.]*' | cut -d= -f2 | head -1)
fi
# iw 拿不到协议时，按频段+速率推断：5G 速率 >150Mbps 只可能是 802.11ac VHT
if [ -z "$PROTO" ] && [ -n "${TX_RATE:-}" ]; then
  PROTO=$(awk -v r="$TX_RATE" -v f="${FREQ:-0}" 'BEGIN{
    if (f>=5000 && r>150) print "802.11ac (按速率推断)";
    else if (f>=5000 && r>54) print "802.11n (按速率推断)";
    else if (r>54) print "802.11n (按速率推断)";
    else print "802.11a/b/g (按速率推断)";
  }')
fi
# iw 拿不到频宽时，按 1x1 速率档粗判（标注为推断）
if [ -z "$WIDTH" ] && [ -n "${TX_RATE:-}" ]; then
  WIDTH=$(awk -v r="$TX_RATE" 'BEGIN{
    if (r>=400) print "80(推断)"; else if (r>=150) print "40(推断)";
    else if (r>0) print "20(推断)"; else print "" }')
fi

echo "SSID      : ${SSID:-未知}"
echo "BSSID     : ${BSSID:-未知}"
echo "频率/频段 : ${FREQ:-?} MHz / $BAND"
echo "信道      : $CHAN"
echo "协议      : ${PROTO:-未知}"
echo "频宽      : ${WIDTH:-未知}${WIDTH:+ MHz}"
echo "RSSI      : ${RSSI:-N/A} dBm  -> $(grade_of "${RSSI:-0}")"
echo "TX 速率   : ${TX_RATE:-N/A} Mbps"
echo "RX 速率   : ${RX_RATE:-N/A}$([ -n "$RX_RATE" ] && echo ' Mbps（station dump 上报）' || echo '（8821cu 不支持 station dump，以 TX 速率为准）')"
echo "$IWC" | grep -q 'Tx-Power' && echo "发射功率  : $(echo "$IWC" | grep -o 'Tx-Power=[-0-9]* dBm' | head -1 | cut -d= -f2)"
echo "$IWC" | grep -q 'Link Quality' && echo "Link Quality: $(echo "$IWC" | grep -o 'Link Quality=[0-9/]*' | cut -d= -f2 | head -1)"

# ------------------------------------------------------------------
sec "5. /proc/net/wireless 原始计数（质量/信号/噪声/重试/丢信标）"
cat /proc/net/wireless 2>/dev/null || echo "(不可读)"
# 列顺序: status link level noise | nwid crypt frag retry misc | beacon
RETRY=$(awk 'NR>2{gsub(/\./,"",$9); print $9}' /proc/net/wireless 2>/dev/null | head -1)
MISC=$(awk 'NR>2{gsub(/\./,"",$10); print $10}' /proc/net/wireless 2>/dev/null | head -1)
BEACON=$(awk 'NR>2{gsub(/\./,"",$11); print $11}' /proc/net/wireless 2>/dev/null | head -1)
echo "累计重传(retry): ${RETRY:-?}   丢弃(misc): ${MISC:-?}   丢失 beacon: ${BEACON:-?}"

# ------------------------------------------------------------------
sec "6. RSSI 稳定性采样（${SAMPLES} 次，每次间隔 1 秒）"
i=0; VALS=""
while [ "$i" -lt "$SAMPLES" ]; do
  r=$(awk 'NR>2{gsub(/\./,"",$4); print $4}' /proc/net/wireless 2>/dev/null | head -1)
  if [ -n "$r" ]; then
    VALS="$VALS
$r"
    printf "%s " "$r"
  else
    printf "N/A "
  fi
  i=$((i+1)); [ "$i" -lt "$SAMPLES" ] && sleep 1
done
echo ""
printf '%s\n' "$VALS" | awk 'NF{
  n++; sum+=$1; if(n==1){mn=$1;mx=$1} if($1<mn) mn=$1; if($1>mx) mx=$1
} END{
  if(n>0){ printf "最小=%d dBm  最大=%d dBm  平均=%.1f dBm\n", mn, mx, sum/n;
    a=sum/n;
    g=(a>=-55)?"好":(a>=-65)?"良好":(a>=-75)?"一般":"很差";
    printf "平均信号评级: %s\n", g;
  } else print "未采集到 RSSI";
}'

# ------------------------------------------------------------------
sec "7. Ping 链路质量（${PING_COUNT} 包 -> ${PING_TARGET:-无}）"
if [ -z "${PING_TARGET:-}" ]; then
  echo "无 ping 目标，跳过"
else
  PING_OUT=$(ping -c "$PING_COUNT" -W "$PING_WAIT" "$PING_TARGET" 2>&1)
  echo "$PING_OUT" | tail -3
  LOSS=$(echo "$PING_OUT" | grep -oE '[0-9]+% packet loss' | grep -oE '[0-9]+')
  RTT=$(echo "$PING_OUT" | grep 'min/avg/max' | sed 's/.*= *//')
  MIN_MS=$(echo "$RTT" | cut -d/ -f1); AVG_MS=$(echo "$RTT" | cut -d/ -f2)
  MAX_MS=$(echo "$RTT" | cut -d/ -f3 | awk '{print $1}')
  echo "丢包率: ${LOSS:-100}%   延迟 min/avg/max: ${MIN_MS:-?}/${AVG_MS:-?}/${MAX_MS:-?} ms"
fi

# ------------------------------------------------------------------
if [ "$DO_SCAN" = "1" ]; then
  sec "8. 周边 AP 扫描（扫描会短暂跳信道，可能掉几个包）"
  if have iw && iw dev $IFACE scan 2>/dev/null | grep -q '^BSS '; then
    iw dev $IFACE scan 2>/dev/null | awk '
      /^BSS / { flush(); b=$2; sub(/\(.*/,"",b); next }
      /^[[:space:]]*freq:/ { f=$2 }
      /^[[:space:]]*signal:/ { r=$2 }
      /^[[:space:]]*SSID:/ { s=$0; sub(/^[[:space:]]*SSID: */,"",s) }
      function flush() { if (b!="") printf "%-18s %5sMHz ch%-4s %5sdBm  %s\n", b, f, ch(f), r, (s==""?"(隐藏)":s); b="";f=0;r="";s="" }
      function ch(x) {
        if (x==2484) return 14;
        if (x>=2412&&x<=2472) return int((x-2407)/5);
        n=split("36 40 44 48 52 56 60 64 100 104 108 112 116 120 124 128 132 136 140 144 149 153 157 161 165",cs," ");
        m=split("5180 5200 5220 5240 5260 5280 5300 5320 5500 5520 5540 5560 5580 5600 5620 5640 5660 5680 5700 5720 5745 5765 5785 5805 5825",fs," ");
        for(i=1;i<=m;i++) if(fs[i]==x) return cs[i]; return "?";
      }
      END { flush() }' | sort -k4 -rn 2>/dev/null | head -30
  elif have iwlist; then
    iwlist $IFACE scan 2>/dev/null | awk '
      /^Cell / { flush(); ap=$NF; next }
      /Frequency:/ { if (match($0,/\(Channel [0-9]+\)/)) { c=substr($0,RSTART+9,RLENGTH-10); } }
      /Signal level=/ { s=$0; sub(/.*Signal level=/,"",s); sub(/ .*/,"",s) }
      /ESSID:/ { e=$0; sub(/.*ESSID:"?/,"",e); sub/"?$/,"",e) }
      function flush() { if(ap!=""){ band=(c<=14)?"2.4G":"5G"; printf "%-18s ch%-4s %-4s %5sdBm  %s\n", ap, c, band, s, (e==""?"(隐藏)":e); } ap="";c="";s="";e="" }
      END { flush() }' | head -30
  else
    echo "!! iw / iwlist 均不可用，无法扫描"
  fi
fi

# ------------------------------------------------------------------
sec "汇总"
echo "SSID=$SSID  BAND=$BAND  CH=$CHAN  WIDTH=${WIDTH:-?}MHz  PROTO=${PROTO:-?}"
echo "RSSI=${RSSI:-N/A}dBm($(grade_of "${RSSI:-0}"))  TX=${TX_RATE:-?}Mbps  RX=${RX_RATE:-?}Mbps"
[ -n "${LOSS:-}" ] && echo "PING ${PING_TARGET}: loss=${LOSS}% avg=${AVG_MS:-?}ms"
hr
echo "判读提示:"
echo "  - 3 米视距 5G 正常应为 -35~-55dBm、TX 200Mbps 以上；若 <-75dBm / 低速档，查天线"
echo "  - ACAG0301-2450-T 是纯 2.4G 天线，5G 失谐会直接导致 RSSI 差、吞吐极低"
echo "  - USB speed 非 480M、retry 持续增长、频宽停在 20MHz 都需逐项排查"
