################################################################################
#
# encodermain — 编码器主控进程（C 实现）
#
# 与 encalertd 同款 generic-package 模式：
#   1. EXTRACT：把本地 src/* 复制到编译目录
#   2. BUILD  ：用 buildroot 提供的 TARGET_CC 编译
#   3. INSTALL：二进制 -> /usr/sbin/encodermain
#
# 注意（项目约束）：测试完成前不进固件默认编译（Config.in 为 default n）；
# 启动入口仍由 overlay 的 start_encoder.sh 控制，可经 ENCODER_MAIN_BIN
# 环境变量选用 /mnt/mmcblk0p1/bin/encodermain 快速验证并一键回滚 bash 版。
#
################################################################################

ENCODERMAIN_LICENSE = Public Domain

define ENCODERMAIN_EXTRACT_CMDS
	cp -avr $(ENCODERMAIN_PKGDIR)/src/*.c $(ENCODERMAIN_PKGDIR)/src/*.h $(@D)/
endef

ENCODERMAIN_MAKE_OPTS = \
	CC="$(TARGET_CC)"

define ENCODERMAIN_BUILD_CMDS
	$(MAKE) $(ENCODERMAIN_MAKE_OPTS) -C $(@D)
endef

define ENCODERMAIN_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/encodermain $(TARGET_DIR)/usr/sbin/encodermain
endef

$(eval $(generic-package))
