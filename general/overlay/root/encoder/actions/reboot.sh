#!/bin/sh
# =============================================================
# reboot.sh — 整机重启（encalertd 重启风暴 6104 fatal 后调用）
#
# 由 encalertd action 执行器调起；测试环境用 fake 替换记录被调。
# reboot 前的报警 flush 已在 C 侧（cb_proc_restart）完成。
# 输出: {"rebooted":1}
# =============================================================
sync
reboot -f
printf '{"rebooted":1}'
exit 0
