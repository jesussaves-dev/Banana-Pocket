################################################################################
#
# bananad
#
################################################################################

BANANAD_VERSION = local
BANANAD_SITE = $(BR2_EXTERNAL_BANANA_PATH)/../system/bananad
BANANAD_SITE_METHOD = local
BANANAD_LICENSE = MIT
BANANAD_DEPENDENCIES = alsa-lib
BANANAD_INSTALL_STAGING = YES

define BANANAD_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -B -C $(@D) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

define BANANAD_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) install
endef

define BANANAD_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/include/banana-retro-control.h \
		$(STAGING_DIR)/usr/include/banana-retro-control.h
endef

$(eval $(generic-package))
