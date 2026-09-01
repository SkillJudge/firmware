#!/bin/sh
# ======================================================================
# hw_probe.sh v2 — 编码器硬件全量自检 (产线人工 + 无人值守两用)
#
# 用法:
#   sh hw_probe.sh                 # 纯自动项 (无人值守安全, 无 LED/声音)
#   sh hw_probe.sh --manual        # 含 LED 扫描 + 声音播放 (需人工看/听)
#
# 输出: 行式报告 HWCHK name=status ev=[evidence]
# 退出码: 0=PASS 1=FAIL 2=WARN
#
# 分层 (116 案例教训: "Cannot start SDK" != 硬件坏):
#   L1 内核/硬件层 = sensor(i2c/vi帧率) / i2c_bus / gpio / sd / audio_node
#   L2 SDK 层      = majestic_sdk (混合软硬件, FAIL 只算 WARN)
#
# 证据源策略 (v2 实测修正):
#   - sensor 运行期: /proc/umap/vi PIPE STATUS FrameRate>0 (dmesg 会被冲掉, 不可靠)
#   - sensor 开机时序 (SDK 未加载): dmesg sensor= 证据
#   - mmz: /proc/media-mem (v1 的 /proc/umap/mmz 是错误路径)
# ======================================================================
set -u

MANUAL=0
[ "${1:-}" = "--manual" ] && MANUAL=1

OK_N=0; FAIL_N=0; WARN_N=0; UNK_N=0; MAN_N=0

check() {
  echo "HWCHK $1=$2 ev=[$3]"
  case "$2" in
    OK)     OK_N=$((OK_N+1)) ;;
    FAIL)   FAIL_N=$((FAIL_N+1)) ;;
    WARN)   WARN_N=$((WARN_N+1)) ;;
    MANUAL) MAN_N=$((MAN_N+1)) ;;
    *)      UNK_N=$((UNK_N+1)) ;;
  esac
}
report() { echo "HWINFO $*"; }

MAJ_RUN=0
pidof majestic >/dev/null 2>&1 && MAJ_RUN=1

# ======================================================================
# 0. 设备身份
# ======================================================================
DID=$(fw_printenv -n DEVICE_ID 2>/dev/null)
[ -n "$DID" ] && check "device_id" "OK" "$DID" || check "device_id" "FAIL" "env 无 DEVICE_ID"
report "hw_probe v4 device=$DID manual=$MANUAL majestic_running=$MAJ_RUN date=$(date '+%F %T')"

# ======================================================================
# 1. L1: 传感器硬件链路
#    运行期: /proc/umap/vi 帧率 (最强证据, 传感器真实出帧)
#    开机时序: dmesg probe 证据
# ======================================================================
if [ "$MAJ_RUN" = "1" ]; then
  VI_LINE=$(grep -A2 "VI PIPE STATUS" /proc/umap/vi 2>/dev/null | tail -1)
  FR=$(echo "$VI_LINE" | awk '{print $4}')
  LOST=$(echo "$VI_LINE" | awk '{print $5}')
  if [ -n "$FR" ] && [ "$FR" -gt 0 ] 2>/dev/null; then
    check "sensor_live" "OK" "vi Pipe0 FrameRate=${FR} LostFrame=${LOST} (传感器出帧中)"
  else
    check "sensor_live" "FAIL" "majestic 运行但 vi 无帧率输出: [$VI_LINE]"
  fi
  # dmesg 证据仅作补充 (可能被冲掉, 不计入判定)
  DM=$(dmesg 2>/dev/null | grep -oE "sensor=[a-z0-9]+" | tail -1)
  [ -n "$DM" ] && report "dmesg_sensor_evidence=$DM (补充信息, 环形缓冲可能已冲掉)"
else
  DM=$(dmesg 2>/dev/null | grep -oE "sensor=[a-z0-9]+" | tail -1)
  DM_OK=$(dmesg 2>/dev/null | grep -cE "sensor_spi.ko OK")
  if [ -n "$DM" ] && [ "$DM_OK" -gt 0 ]; then
    check "sensor_boot_probe" "OK" "$DM load_ok=$DM_OK"
  elif [ -n "$DM" ]; then
    check "sensor_boot_probe" "WARN" "$DM 无 load OK 证据"
  else
    check "sensor_boot_probe" "UNKNOWN" "SDK 未运行且 dmesg 无 sensor 证据 (可能被冲掉)"
  fi
fi

# ======================================================================
# 2. L1: I2C 总线与器件应答 (read 模式; 0x20=PCF8574, 0x36=副器件)
# ======================================================================
if command -v i2cdetect >/dev/null 2>&1; then
  I2C1=$(i2cdetect -y -r 1 2>/dev/null)
  PCF=$(echo "$I2C1" | grep -E "^20:" | grep -cE "(^| )20( |$)")
  DEV36=$(echo "$I2C1" | grep -E "^30:" | grep -cE "(^| )36( |$)")
  if [ "$PCF" -ge 1 ] && [ "$DEV36" -ge 1 ]; then
    check "i2c_bus1_devices" "OK" "0x20(PCF8574)+0x36 应答"
  elif [ "$PCF" -ge 1 ] || [ "$DEV36" -ge 1 ]; then
    check "i2c_bus1_devices" "WARN" "部分应答 pcf0x20=$PCF dev0x36=$DEV36"
  else
    check "i2c_bus1_devices" "FAIL" "bus1 无已知器件应答 (I2C 控制器/上拉/器件故障)"
  fi
else
  check "i2c_bus1_devices" "UNKNOWN" "无 i2cdetect"
fi

# ======================================================================
# 3. L1: sensor 配置文件 (majestic 误检 imx347 配套 ini)
# ======================================================================
SCFG=""
for f in /etc/sensors/imx347_i2c_4M.ini /etc/sensors/imx347_i2c.ini; do
  [ -f "$f" ] && SCFG="$f" && break
done
[ -n "$SCFG" ] && check "sensor_config" "OK" "$SCFG" || check "sensor_config" "FAIL" "/etc/sensors 无 imx347*.ini"

# ======================================================================
# 4. L2: majestic SDK 状态 (混合软硬件, 只记 WARN)
#    v3: 以 /proc/umap/vi 存在性为主证据 (logread 环形缓冲会被冲掉)
# ======================================================================
if [ "$MAJ_RUN" = "1" ]; then
  if [ -e /proc/umap/vi ]; then
    M_LOG=$(logread 2>/dev/null | grep majestic | grep -E "SDK started|Cannot start SDK" | tail -1)
    check "majestic_sdk" "OK" "进程+SDK 节点(/proc/umap/vi)正常 log=[${M_LOG##*: }]"
  else
    check "majestic_sdk" "WARN" "进程在但 SDK 节点缺失 (Cannot start SDK? 软硬件混合)"
  fi
else
  check "majestic_sdk" "UNKNOWN" "majestic 未运行 (进程层由 encalertd 6103 负责)"
fi

# ======================================================================
# 5. L1: MMZ 媒体内存 (/proc/media-mem, v2 修正路径)
# ======================================================================
if [ -e /proc/media-mem ]; then
  ZONE=$(grep -m1 "ZONE:" /proc/media-mem 2>/dev/null)
  check "mmz_mem" "OK" "$(echo "$ZONE" | sed 's/^ *//' | head -c 90)"
elif [ "$MAJ_RUN" = "1" ]; then
  check "mmz_mem" "FAIL" "majestic 运行但 /proc/media-mem 缺失"
else
  check "mmz_mem" "UNKNOWN" "SDK 未加载, media-mem 不存在——非故障"
fi

# ======================================================================
# 6. L1: 音频硬件 (/dev/acodec + ao 状态)
# ======================================================================
if [ -e /dev/acodec ]; then
  if [ "$MAJ_RUN" = "1" ] && [ -e /proc/umap/ao ]; then
    AO_INT=$(grep -A3 "AO DEV STATUS0" /proc/umap/ao 2>/dev/null | tail -1 | awk '{print $2}')
    if [ -n "$AO_INT" ] && [ "$AO_INT" -gt 0 ] 2>/dev/null; then
      check "audio_hw" "OK" "/dev/acodec + ao IntCnt=$AO_INT (输出通道活动)"
    else
      check "audio_hw" "OK" "/dev/acodec 存在 (ao 无活动, 无音频业务属正常)"
    fi
  else
    check "audio_hw" "OK" "/dev/acodec 存在"
  fi
else
  check "audio_hw" "FAIL" "/dev/acodec 缺失"
fi

# ======================================================================
# 7. L1: SD 卡 + 网络 + GPIO
# ======================================================================
if [ -e /dev/mmcblk0p1 ]; then
  if mount | grep -q mmcblk0p1; then
    TFILE=/mnt/mmcblk0p1/.hwprobe_$$_test
    if echo "hwprobe" > "$TFILE" 2>/dev/null && grep -q hwprobe "$TFILE" 2>/dev/null; then
      rm -f "$TFILE"
      check "sdcard_rw" "OK" "挂载+读写探针通过"
    else
      check "sdcard_rw" "FAIL" "已挂载但读写探针失败"
    fi
  else
    check "sdcard_rw" "FAIL" "节点存在但未挂载"
  fi
else
  check "sdcard_rw" "FAIL" "/dev/mmcblk0p1 不存在"
fi

ETH_C=$(cat /sys/class/net/eth0/carrier 2>/dev/null)
[ "$ETH_C" = "1" ] && check "eth_link" "OK" "carrier=1" || check "eth_link" "WARN" "carrier=${ETH_C:-0} (WiFi 场景未插线属正常)"
[ -e /sys/class/net/wlan0 ] && check "wlan_present" "OK" "wlan0" || check "wlan_present" "FAIL" "无 wlan0"

GPIO_N=$(ls /dev/gpiochip* 2>/dev/null | wc -l)
[ "$GPIO_N" -ge 5 ] && check "gpio_chips" "OK" "count=$GPIO_N" || check "gpio_chips" "WARN" "count=$GPIO_N"

# ======================================================================
# 8. 资源摸底 (人工项准备)
# ======================================================================
LED_TOOL=""
command -v led_test >/dev/null 2>&1 && LED_TOOL="led_test"
[ -z "$LED_TOOL" ] && [ -x /usr/bin/led_test ] && LED_TOOL="/usr/bin/led_test"
[ -n "$LED_TOOL" ] && check "led_tool" "OK" "$LED_TOOL" || check "led_tool" "UNKNOWN" "无 led_test"

# ======================================================================
# 9. 人工项 (仅 --manual)
# ======================================================================
if [ "$MANUAL" = "1" ]; then
  if [ -n "$LED_TOOL" ]; then
    report "LED 扫描: 请目视, 每盏约 2s"
    OUT=$("$LED_TOOL" 2>&1 | tail -2)
    report "led_scan 输出: $OUT"
    check "led_scan" "MANUAL" "已执行, 待人工确认 LED"
  else
    check "led_scan" "UNKNOWN" "无工具"
  fi

  # 板载语音播放: 对齐 bash voice.sh (Majestic 内置 /play_audio 接口)
  # 注意: .audio.enabled 创建 ADEC/AO 通道 + .audio.outputEnabled 开放 HTTP 接口,
  #       两者缺一则 /play_audio 返回 200 但无声 (ERR_ADEC_UNEXIST)
  VOICE_PCM=/root/resources/desk_8k.pcm
  if command -v curl >/dev/null 2>&1 && [ -f "$VOICE_PCM" ] && [ "$MAJ_RUN" = "1" ]; then
    report "声音播放: 将播放约 2.8s 板载语音, 请听扬声器/耳机"
    yaml-cli -i /etc/majestic.yaml -s .audio.enabled true >/dev/null 2>&1
    yaml-cli -i /etc/majestic.yaml -s .audio.outputEnabled true >/dev/null 2>&1
    killall -HUP majestic 2>/dev/null
    sleep 2
    if curl -fsS --connect-timeout 3 --max-time 10 --data-binary "@$VOICE_PCM" \
         http://127.0.0.1/play_audio >/dev/null 2>&1; then
      sleep 4
      SR=$(yaml-cli -i /etc/majestic.yaml -g .audio.srate 2>/dev/null)
      CD=$(yaml-cli -i /etc/majestic.yaml -g .audio.codec 2>/dev/null)
      check "audio_play" "MANUAL" "PCM 已提交(curl OK) srate=$SR codec=$CD, 待人工确认有声"
    else
      check "audio_play" "FAIL" "POST /play_audio 失败 (curl rc!=0, majestic HTTP 异常)"
    fi
  else
    check "audio_play" "UNKNOWN" "缺 curl/PCM/majestic (pcm=$([ -f "$VOICE_PCM" ] && echo ok || echo missing))"
  fi
fi

# ======================================================================
# 汇总
# ======================================================================
echo "HWSUMMARY ok=$OK_N fail=$FAIL_N warn=$WARN_N manual=$MAN_N unknown=$UNK_N"
if [ "$FAIL_N" -gt 0 ]; then
  echo "HWVERDICT=FAIL"; exit 1
elif [ "$WARN_N" -gt 0 ]; then
  echo "HWVERDICT=WARN"; exit 2
fi
echo "HWVERDICT=PASS"
exit 0
