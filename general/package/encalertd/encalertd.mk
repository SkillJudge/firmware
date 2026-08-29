################################################################################
#
# encalertd — 编码器报警守护进程（C 实现）
#
# 与 factoryinit 同款 generic-package 模式：
#   1. EXTRACT：把本地 src/* 复制到编译目录
#   2. BUILD  ：用 buildroot 提供的 TARGET_CC 编译
#   3. INSTALL：二进制 -> /usr/sbin/encalertd，启动脚本 -> /etc/init.d/S43encalertd
#
################################################################################

ENCALLOCERTD_LICENSE = Public Domain

define ENCALLOCERTD_EXTRACT_CMDS
	cp -avr $(ENCALLOCERTD_PKGDIR)/src/*.c $(ENCALLOCERTD_PKGDIR)/src/*.h $(@D)/
endef

ENCALLOCERTD_MAKE_OPTS = \
	CC="$(TARGET_CC)"

define ENCALLOCERTD_BUILD_CMDS
	$(MAKE) $(ENCALLOCERTD_MAKE_OPTS) -C $(@D)
endef

define ENCALLOCERTD_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/encalertd $(TARGET_DIR)/usr/sbin/encalertd
	$(INSTALL) -m 0755 -D $(ENCALLOCERTD_PKGDIR)/S43encalertd $(TARGET_DIR)/etc/init.d/S43encalertd
endef

$(eval $(generic-package))
