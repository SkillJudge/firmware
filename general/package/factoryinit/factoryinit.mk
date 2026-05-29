################################################################################
#
# factoryinit
#
################################################################################

FACTORYINIT_LICENSE = Public Domain

# 1. 像 xmdp 一样，在解压阶段直接把本地 src 里的代码复制到编译目录
define FACTORYINIT_EXTRACT_CMDS
	cp -avr $(FACTORYINIT_PKGDIR)/src/* $(@D)/
endef

# 2. 传入系统交叉编译器
FACTORYINIT_MAKE_OPTS = \
	CC="$(TARGET_CC)"

# 3. 编译阶段
define FACTORYINIT_BUILD_CMDS
	$(MAKE) $(FACTORYINIT_MAKE_OPTS) -C $(@D)
endef

# 4. 安装阶段：把程序塞进 /usr/bin/，同时把自启脚本塞进 /etc/init.d/
define FACTORYINIT_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/ipc_server $(TARGET_DIR)/usr/bin/ipc_server
	$(INSTALL) -m 0755 -D $(FACTORYINIT_PKGDIR)/S99factoryinit $(TARGET_DIR)/etc/init.d/S99factoryinit
endef

$(eval $(generic-package))