################################################################################
#
# encalertd — 编码器报警守护进程（C 实现）
#
# 与 factoryinit 同款 generic-package 本地包模式：
#   1. SITE_METHOD=local：把本地 src/ 复制到编译目录
#   2. BUILD            ：用 buildroot 提供的 TARGET_CC 编译
#   3. INSTALL          ：二进制 -> /usr/sbin/encalertd
#                         自启脚本 -> /etc/init.d/S43encalertd
#
# 注意：变量前缀必须与包名一致（ENCALERTD_*）。
# 2026-08-31 修正：原 ENCALLOCERTD 拼写使钩子不被 generic-package 识别。
#
################################################################################

ENCALERTD_VERSION = 3.2.0
ENCALERTD_SITE = $(ENCALERTD_PKGDIR)/src
ENCALERTD_SITE_METHOD = local
ENCALERTD_LICENSE = Public Domain

ENCALERTD_MAKE_OPTS = \
	CC="$(TARGET_CC)"

define ENCALERTD_BUILD_CMDS
	$(MAKE) $(ENCALERTD_MAKE_OPTS) -C $(@D)
endef

define ENCALERTD_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/encalertd $(TARGET_DIR)/usr/sbin/encalertd
	$(INSTALL) -m 0755 -D $(ENCALERTD_PKGDIR)/S43encalertd $(TARGET_DIR)/etc/init.d/S43encalertd
endef

$(eval $(generic-package))
