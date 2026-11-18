################################################################################
#
# pokedaisy
#
################################################################################

# PokeDaisy's Linux host (pokedaisy-linux, SDL2) and its GB/GBC (mGBA) and GBA (armGBA) libretro cores. Upstream pins
# the emulators as git submodules; the ones this build needs (mgba, armgba, zlib) are fetched as tarballs at the
# commits upstream pins, so the huge unused submodules (azahar, melonds ...) never download. The DS core is not built.
POKEDAISY_VERSION = 79bd00e155fcc7d4373f23f5386d6e67c8c423eb
POKEDAISY_SITE = $(call github,lidor30,pokedaisy,$(POKEDAISY_VERSION))
POKEDAISY_MGBA_VERSION = 26b7884bc25a5933960f3cdcd98bac1ae14d42e2
POKEDAISY_ARMGBA_VERSION = d528d09e750cb781c476a3a21d567fb227679578
POKEDAISY_ZLIB_VERSION = 51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf
POKEDAISY_EXTRA_DOWNLOADS = \
	https://github.com/mgba-emu/mgba/archive/$(POKEDAISY_MGBA_VERSION).tar.gz \
	https://github.com/ratherDashing/armGBA/archive/$(POKEDAISY_ARMGBA_VERSION).tar.gz \
	https://github.com/madler/zlib/archive/$(POKEDAISY_ZLIB_VERSION).tar.gz
POKEDAISY_LICENSE = GPL-3.0+
POKEDAISY_LICENSE_FILES = LICENSE NOTICE
POKEDAISY_DEPENDENCIES = sdl2 host-cmake

define POKEDAISY_EXTRACT_SUBMODULES
	$(foreach m,mgba:MGBA armgba:ARMGBA zlib:ZLIB, \
		mkdir -p $(@D)/third_party/$(word 1,$(subst :, ,$(m))) && \
		tar -xzf $(POKEDAISY_DL_DIR)/$(POKEDAISY_$(word 2,$(subst :, ,$(m)))_VERSION).tar.gz \
			--strip-components=1 -C $(@D)/third_party/$(word 1,$(subst :, ,$(m))) &&) true
endef
POKEDAISY_POST_EXTRACT_HOOKS += POKEDAISY_EXTRACT_SUBMODULES

POKEDAISY_CMAKE = $(HOST_DIR)/bin/cmake -DCMAKE_TOOLCHAIN_FILE=$(HOST_DIR)/share/buildroot/toolchainfile.cmake \
	-DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5

define POKEDAISY_BUILD_CMDS
	$(POKEDAISY_CMAKE) -S $(@D)/libretro -B $(@D)/build/gbc
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/build/gbc pokedaisy_gbc_libretro
	$(POKEDAISY_CMAKE) -S $(@D)/libretro/armgba -B $(@D)/build/gba
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/build/gba pokedaisy_gba_libretro
	$(POKEDAISY_CMAKE) -S $(@D)/linux -B $(@D)/build/host
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/build/host pokedaisy-linux
endef

define POKEDAISY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/host/pokedaisy-linux $(TARGET_DIR)/usr/bin/pokedaisy-linux
	$(INSTALL) -D -m 0755 $(@D)/build/gbc/pokedaisy_gbc_libretro.so $(TARGET_DIR)/usr/lib/pokedaisy/pokedaisy_gbc_libretro.so
	$(INSTALL) -D -m 0755 $(@D)/build/gba/pokedaisy_gba_libretro.so $(TARGET_DIR)/usr/lib/pokedaisy/pokedaisy_gba_libretro.so
	$(INSTALL) -D -m 0644 $(BR2_EXTERNAL_BANANA_PATH)/../app/pokedaisy/app.json $(TARGET_DIR)/usr/share/banana/apps/pokedaisy/app.json
	$(INSTALL) -D -m 0644 $(BR2_EXTERNAL_BANANA_PATH)/../app/pokedaisy/PokeDaisy.app $(TARGET_DIR)/usr/share/banana/apps/pokedaisy/PokeDaisy.app
	$(INSTALL) -D -m 0644 $(@D)/NOTICE $(TARGET_DIR)/usr/share/banana/apps/pokedaisy/NOTICE
endef

$(eval $(generic-package))
