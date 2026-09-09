#!/bin/sh
# =============================================================
# reboot.sh — 整机重启（encalertd 重启风暴 6104 fatal 后调用）
#
# 由 encalertd action 执行器调起；测试环境用 fake 替换记录被调。
# reboot 前的报警 flush 已在 C 侧（cb_proc_restart）完成。
# 输出: {"rebooted":1}
# =============================================================
sync
# 优先 busybox reboot；若 rootfs 正处于升级后不一致状态
# （flashcp 已覆盖但未重启），busybox 可能读失败，
# 用 sysrq-trigger 直接让内核重启，不依赖任何 flash 读取。
reboot -f 2>/dev/null || echo b > /proc/sysrq-trigger
printf '{"rebooted":1}'
exit 0
