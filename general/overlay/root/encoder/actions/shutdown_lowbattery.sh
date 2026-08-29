#!/bin/sh
#
# shutdown_lowbattery.sh — 低电保护关机（硬件对齐版）
#
# 本机电源硬件（参考 /usr/bin/power_key_test、/usr/bin/led_test.sh）：
#   PCF8574 @ I2C1/0x20：
#     P0=CHRG(充)  P1=STDBY  P2=软关机引脚(必须常高，拉低>=2s 触发硬件断电)
#     P3=LED1      P4=LED2A   P5=LED2B
#   Linux `poweroff` 只能停系统，真正断电靠 P2 拉低触发关机电路。
#
# 执行顺序：
#   1. sync 落盘 + hook 挂点
#   2. 点亮 LED1+LED2B 指示即将断电（失败不阻塞）
#   3. 后台 2s 后 poweroff（系统收尾尽力而为）
#   4. 前台拉低 P2 并保持 —— 硬件关机电路检测到持续低电平后彻底断电
#   5. 若关机电路未响应，后台 poweroff 兜底至少 halt 系统
#
# 输出单行 JSON。
PATH=/usr/sbin:/usr/bin:/sbin:/bin

ENCODER_CONFIG="${ENCODER_CONFIG:-/root/encoder/config.sh}"
[ -f "$ENCODER_CONFIG" ] && . "$ENCODER_CONFIG" 2>/dev/null

I2C_BUS="${POWER_I2C_BUS:-1}"
I2C_ADDR="${POWER_I2C_ADDR:-0x20}"

echo '{"rc":0,"note":"shutdown_begin"}'

sync

# --- LED 关机指示：LED1(P3)+LED2B(P5) 常亮；强制 P0/P1/P2 保护位为高 ---
# 失败（I2C 异常）不影响后续关机流程
led_shutdown_hint() {
    local cur
    cur=$(i2cget -y "$I2C_BUS" "$I2C_ADDR" 2>/dev/null) || return 0
    i2cset -y "$I2C_BUS" "$I2C_ADDR" \
        "0x$(printf '%02x' $(( (cur | (1<<3) | (1<<5) | 0x07) & 0xFF )))" \
        >/dev/null 2>&1 || true
}
led_shutdown_hint

# --- hook 挂点（保留原行为） ---
if [ -n "${CUSTOM_SHUTDOWN_HOOK:-}" ] && [ -x "${CUSTOM_SHUTDOWN_HOOK}" ]; then
    ( "${CUSTOM_SHUTDOWN_HOOK}" "low_battery" ) >/dev/null 2>&1 || true
fi

if [ -n "${LOW_BATTERY_SHUTDOWN_SCRIPT:-}" ] && [ -x "${LOW_BATTERY_SHUTDOWN_SCRIPT}" ]; then
    nohup "${LOW_BATTERY_SHUTDOWN_SCRIPT}" >/tmp/low_battery_shutdown.log 2>&1 &
fi

sync

# --- 后台 poweroff：系统收尾与 P2 拉低并行；若进程已断电则无所谓 ---
( sleep 2; poweroff -f 2>/dev/null || poweroff ) >/dev/null 2>&1 &

# --- 前台 P2 软关机：拉低并保持至硬件断电（写后不再恢复） ---
# 寄存器值在系统消失后保持不变，关机电路检测到 P2 持续低电平即断电。
p2_power_off() {
    local cur
    cur=$(i2cget -y "$I2C_BUS" "$I2C_ADDR" 2>/dev/null) || return 1
    i2cset -y "$I2C_BUS" "$I2C_ADDR" \
        "0x$(printf '%02x' $(( cur & ~(1<<2) & 0xFF )))" \
        >/dev/null 2>&1 || return 1
    return 0
}
sleep 1
if p2_power_off; then
    echo '{"rc":0,"note":"p2_shutdown_asserted"}'
else
    echo '{"rc":0,"note":"p2_unavailable_fallback_poweroff"}'
fi

# 等待硬件断电；若关机电路未触发，后台 poweroff 已兜底
sleep 5
exit 0
