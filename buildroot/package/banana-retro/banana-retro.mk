################################################################################
#
# banana-retro
#
################################################################################

BANANA_RETRO_VERSION = local
BANANA_RETRO_SITE = $(BR2_EXTERNAL_BANANA_PATH)/../system/banana-retro
BANANA_RETRO_SITE_METHOD = local
BANANA_RETRO_LICENSE = MIT
BANANA_RETRO_LICENSE_FILES = LICENSE include/libretro.h
BANANA_RETRO_DEPENDENCIES = alsa-lib bananad wayland wayland-protocols host-pkgconf host-wayland

define BANANA_RETRO_COPY_LICENSE
	cp $(BR2_EXTERNAL_BANANA_PATH)/../LICENSE $(@D)/LICENSE
endef
BANANA_RETRO_POST_RSYNC_HOOKS += BANANA_RETRO_COPY_LICENSE

define BANANA_RETRO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -B -C $(@D) \
		CC="$(TARGET_CC)" PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
		WAYLAND_SCANNER="$(HOST_DIR)/bin/wayland-scanner" \
		WAYLAND_PROTOCOLS="$(STAGING_DIR)/usr/share/wayland-protocols" \
		CONTROL_HEADER="$(STAGING_DIR)/usr/include/banana-retro-control.h" \
		CONTROL_CFLAGS="-I$(STAGING_DIR)/usr/include" \
		CFLAGS="$(TARGET_CFLAGS) -O2" LDFLAGS="$(TARGET_LDFLAGS)" \
		all
endef

define BANANA_RETRO_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) DESTDIR=$(TARGET_DIR) PREFIX=/usr \
		SYSCONFDIR=/etc CONTROL_HEADER="$(STAGING_DIR)/usr/include/banana-retro-control.h" \
		CONTROL_CFLAGS="-I$(STAGING_DIR)/usr/include" install
endef

$(eval $(generic-package))
