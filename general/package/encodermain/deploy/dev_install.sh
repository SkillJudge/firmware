#!/bin/sh
# ======================================================================
# dev_install.sh — deploy.ps1 的板端安装子脚本 (纯 sh, ash 兼容)
# 前置: /tmp/*.new 已由 pscp 上传
# 动作: mv → /usr/sbin + /mnt/mmcblk0p1/bin (双路径) + init.d 覆盖 +
#       encalertd.conf 仅当缺失时落盘 + stop/start S43 + S96 + VERIFY
# ======================================================================
set +e
BIN_SD=/mnt/mmcblk0p1/bin
mkdir -p "$BIN_SD" /etc/init.d /usr/sbin /etc 2>/dev/null

# 1. 升级二进制 (usr/sbin + SD 双份)
mv -f /tmp/encodermain.new    /usr/sbin/encodermain
mv -f /tmp/encalertd.new      /usr/sbin/encalertd
cp -f /usr/sbin/encodermain   "$BIN_SD/encodermain" 2>/dev/null
cp -f /usr/sbin/encalertd     "$BIN_SD/encalertd"   2>/dev/null
chmod 755 /usr/sbin/encodermain /usr/sbin/encalertd 2>/dev/null
chmod 755 "$BIN_SD/encodermain" "$BIN_SD/encalertd" 2>/dev/null

# 2. init.d 脚本
mv -f /tmp/S96encodermain.new /etc/init.d/S96encodermain
mv -f /tmp/S43encalertd.new   /etc/init.d/S43encalertd
chmod 755 /etc/init.d/S96encodermain /etc/init.d/S43encalertd

# 3. encalertd 配置 (仅缺失时落模板, 不覆盖已有个性化)
if [ -s /tmp/encalertd.conf.new ] && [ ! -s /etc/encalertd.conf ]; then
  mv -f /tmp/encalertd.conf.new /etc/encalertd.conf
  chmod 644 /etc/encalertd.conf
else
  rm -f /tmp/encalertd.conf.new
fi

echo ===MD5===
md5sum /usr/sbin/encodermain /usr/sbin/encalertd 2>/dev/null

echo ===START===
pidof encalertd    >/dev/null 2>&1 && /etc/init.d/S43encalertd stop    >/dev/null 2>&1
pidof encodermain  >/dev/null 2>&1 && /etc/init.d/S96encodermain stop   >/dev/null 2>&1
# NOTE: SIGKILL'ed busybox tasks can stay visible in /proc/*/comm for ~2s
# (D/EXIT-Z status); encodermain single-instance guard walks /proc so we
# MUST wait long enough OR retry S96 start; here we combine: wait 3s,
# remove stale pidfiles, S43 then S96, retry S96 once if pid empty.
sleep 3
killall -9 encodermain encalertd 2>/dev/null
rm -f /root/encoder/runtime/state/encodermain.pid /var/run/encalertd.pid
/etc/init.d/S43encalertd start   2>&1 | head -n 3
/etc/init.d/S96encodermain start 2>&1 | head -n 3
sleep 6
# Retry S96 once if encodermain didn't come up (known 2s residual comm race)
if ! pidof encodermain >/dev/null 2>&1; then
  echo "RETRY S96 (comm residual race)"
  rm -f /root/encoder/runtime/state/encodermain.pid
  sleep 2
  /etc/init.d/S96encodermain start 2>&1 | head -n 3
  sleep 5
fi

echo ===VERIFY===
echo "pidof encodermain: $(pidof encodermain 2>/dev/null)"
echo "pidof encalertd  : $(pidof encalertd   2>/dev/null)"
PIDF=/root/encoder/runtime/state/encodermain.pid
if [ -f "$PIDF" ]; then echo "state/encodermain.pid=$(cat "$PIDF")"; else echo "state/encodermain.pid=MISSING"; fi
PIDFA=/var/run/encalertd.pid
if [ -f "$PIDFA" ]; then echo "encalertd.pid=$(cat "$PIDFA")"; else echo "encalertd.pid=MISSING"; fi
pc=$(pidof encodermain 2>/dev/null | wc -w)
if [ "$pc" -gt 1 ]; then echo "WARN encodermain multi-instance (count=$pc)"; fi
ac=$(pidof encalertd 2>/dev/null | wc -w)
if [ "$ac" -gt 1 ]; then echo "WARN encalertd multi-instance (count=$ac)"; fi

echo ===MAJESTIC===
if pidof majestic >/dev/null 2>&1; then
  echo "majestic running pid=$(pidof majestic)"
else
  echo "majestic stopped (S95majestic 自恢复 or 推流时 recover)"
fi

echo ===DEVICE_ID===
fw_printenv DEVICE_ID 2>/dev/null || echo DEVICE_ID_MISSING

echo ===ENC_LOG_TAIL===
tail -n 3 /root/encoder/runtime/logs/encoder.log 2>/dev/null
echo ===ALERT_LOG_TAIL===
tail -n 3 /tmp/encalertd.log 2>/dev/null

echo DEPLOY_DONE
