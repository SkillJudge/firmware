#!/bin/sh
# WiFi 硬件/软件故障诊断脚本
# 在板端串口执行：sh /root/encoder/wifi_hw_diag.sh
# 或直接贴整段命令到串口执行

echo "========== WiFi 硬件/软件诊断 =========="
echo "时间: $(date)"
echo ""

echo "===== 1. 内核版本/设备树 ====="
uname -r
cat /proc/device-tree/model 2>/dev/null || echo "(no device-tree model)"
echo ""

echo "===== 2. USB 主控制器 ====="
ls /sys/bus/usb/devices/ 2>&1
echo "--- USB 主控制器数量 ---"
ls -d /sys/bus/usb/devices/usb* 2>&1
echo ""

echo "===== 3. USB 设备清单（lsusb） ====="
lsusb 2>&1
echo ""
echo "--- 期望看到 Realtek(0bda:) 或 8821cu(c821:) ---"
lsusb 2>&1 | grep -iE "0bda|c821|realtek|8821" || echo "未发现 Realtek/8821cu USB 设备"
echo ""

echo "===== 4. USB 设备树详情 ====="
for d in /sys/bus/usb/devices/*; do
    [ -d "$d" ] || continue
    echo "--- $d ---"
    echo "  vendor:    $(cat $d/idVendor 2>/dev/null)"
    echo "  product:   $(cat $d/idProduct 2>/dev/null)"
    echo "  manufacturer: $(cat $d/manufacturer 2>/dev/null)"
    echo "  product_name:  $(cat $d/product 2>/dev/null)"
    echo "  authorized: $(cat $d/authorized 2>/dev/null)"
    echo "  bcdDevice: $(cat $d/bcdDevice 2>/dev/null)"
done
echo ""

echo "===== 5. USB 端口电源/使能状态 ====="
echo "--- USB 端口 power/active ---"
for d in /sys/bus/usb/devices/usb*; do
    [ -d "$d" ] || continue
    echo "  $d:"
    echo "    authorized: $(cat $d/authorized 2>/dev/null)"
    echo "    autosuspend: $(cat $d/power/autosuspend_delay_ms 2>/dev/null)"
    echo "    control: $(cat $d/power/control 2>/dev/null)"
done
echo ""

echo "===== 6. WiFi 驱动模块状态 ====="
echo "--- 已加载模块 ---"
lsmod | grep -iE "8821|cfg80211|usbcore|rfkill" 2>&1
echo ""
echo "--- 8821cu.ko 文件是否存在 ---"
find /lib/modules -name "8821cu*" 2>&1
echo ""
echo "--- modinfo 8821cu ---"
modinfo 8821cu 2>&1 | head -15
echo ""

echo "===== 7. 手动 modprobe 8821cu 看报错 ====="
echo "--- 卸载再加载 ---"
rmmod 8821cu 2>&1
sleep 1
modprobe -v 8821cu 2>&1
sleep 2
echo ""
echo "--- modprobe 后 dmesg 最后 20 行 ---"
dmesg | tail -20
echo ""

echo "===== 8. wlan0 接口是否存在 ====="
ls -la /sys/class/net/wlan0 2>&1
ip link show wlan0 2>&1
echo ""
echo "--- /sys/class/net 全部接口 ---"
ls /sys/class/net/ 2>&1
echo ""

echo "===== 9. WiFi 驱动加载脚本测试 ====="
echo "--- /etc/wifi.conf ---"
cat /etc/wifi.conf 2>&1
echo ""
echo "--- 执行 /etc/wireless/usb rtl8811cu-generic ---"
/etc/wireless/usb rtl8811cu-generic 2>&1
echo "exit=$?"
sleep 3
echo ""
echo "--- 执行后 wlan0 是否出现 ---"
ls /sys/class/net/wlan0 2>&1
echo ""

echo "===== 10. USB 总线 dmesg 历史 ====="
echo "--- 启动后 USB 相关日志 ---"
dmesg | grep -iE "usb|8821|wifi|wlan|rtl|realtek" | tail -30
echo ""

echo "===== 11. rfkill 状态 ====="
rfkill list 2>&1
rfkill unblock all 2>&1
echo ""

echo "===== 12. wpa_supplicant 进程 ====="
ps | grep -i wpa 2>&1
echo ""

echo "===== 13. GPIO/电源控制（GK7205V300） ====="
echo "--- 检查是否有 GPIO 控制 WiFi 电源 ---"
ls /sys/class/gpio/ 2>&1 | head -20
echo ""
echo "--- devmem 0x100C0080（USB 复用寄存器） ---"
devmem 0x100C0080 2>&1 || echo "devmem 不可用"
echo ""

echo "===== 14. 完整 USB debugfs（如果可用） ====="
cat /sys/kernel/debug/usb/devices 2>&1 || echo "usb debugfs 不可用"
echo ""

echo "===== 15. 物理检查提示 ====="
echo "如果以上全部看不到 USB 设备："
echo "  1. 检查 WiFi 模组是否焊接牢固（用放大镜看焊点）"
echo "  2. 用万用表量 WiFi 模组 VCC 3.3V 供电"
echo "  3. 用万用表量 USB D+/D- 数据线连通性"
echo "  4. 换一块已知正常的 WiFi 模组交叉测试"
echo ""
echo "===== 诊断完成 ====="
