################################################################################
#
# encodermain — 编码器主控进程（C 实现）
#
# 与 factoryinit 同款 generic-package 本地包模式：
#   1. SITE_METHOD=local：把本地 src/ 复制到编译目录
#   2. BUILD            ：用 buildroot 提供的 TARGET_CC 编译
#   3. INSTALL          ：二进制 -> /usr/sbin/encodermain
#                         自启脚本 -> /etc/init.d/S96encodermain
#                         环境模板 -> /etc/default/encoder
#
# 2026-08-31 起正式进入大循环编译打包（Config.in default y）；
# bash 版启动服务 S99zzencoder 已从 overlay 删除，bash 脚本群文件保留。
#
################################################################################

ENCODERMAIN_VERSION = 3.2.0
ENCODERMAIN_SITE = $(ENCODERMAIN_PKGDIR)/src
ENCODERMAIN_SITE_METHOD = local
ENCODERMAIN_LICENSE = Public Domain

ENCODERMAIN_MAKE_OPTS = \
	CC="$(TARGET_CC)"

define ENCODERMAIN_BUILD_CMDS
	$(MAKE) $(ENCODERMAIN_MAKE_OPTS) -C $(@D)
endef

define ENCODERMAIN_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/encodermain $(TARGET_DIR)/usr/sbin/encodermain
	$(INSTALL) -m 0755 -D $(ENCODERMAIN_PKGDIR)/S96encodermain $(TARGET_DIR)/etc/init.d/S96encodermain
	$(INSTALL) -m 0644 -D $(ENCODERMAIN_PKGDIR)/default.encoder $(TARGET_DIR)/etc/default/encoder
endef

$(eval $(generic-package))
