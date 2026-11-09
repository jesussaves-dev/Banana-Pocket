# banana-ui. Cross builds set CXX (and PKG_CONFIG for the target's sysroot); Buildroot does both.
#   make                 the frontend (needs libdrm, freetype; ALSA=1 by default for the menu sounds)
#   make host            a headless build for this machine (no ALSA), for tests and screenshots
.DEFAULT_GOAL := banana-ui
CXX ?= g++
PKG_CONFIG ?= pkg-config
ALSA ?= 1
CXXFLAGS ?= -O2
override CXXFLAGS += -std=c++17 -Wall -Wno-unused-function
override LDLIBS += -lpthread

SRC = src/main.cpp src/soft.cpp src/kms.cpp src/compositor.cpp src/input.cpp src/library.cpp src/sound.cpp src/splash.cpp src/settings.cpp src/settings_defaults.cpp src/wifi.cpp src/zip.cpp src/theme.cpp src/json.cpp src/i18n.cpp src/expr.cpp src/layout.cpp rnds/RndsRaster.cpp rnds/RndsUI.cpp
PKGS = freetype2 libdrm zlib wayland-server
ifeq ($(ALSA),1)
PKGS += alsa
override CXXFLAGS += -DBANANA_ALSA
endif
override CXXFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
override LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

# Phase 3B: the Wayland server needs xdg-shell, linux-dmabuf and wl_drm generated server code. The scanner comes
# from the build environment's PATH (Buildroot puts its host tools there). The protocol XMLs are architecture
# independent; the sysroot's copy is used when the target pkg-config points at one that exists.
WAYLAND_SCANNER ?= $(shell command -v wayland-scanner 2>/dev/null || echo wayland-scanner)
WAYLAND_PROTOCOLS ?= $(shell d=$$($(PKG_CONFIG) --variable=pkgdatadir wayland-protocols 2>/dev/null); test -f "$$d/stable/xdg-shell/xdg-shell.xml" && echo "$$d" || echo /usr/share/wayland-protocols)
WLDIR = build/wayland
WL_C = $(WLDIR)/xdg-shell-protocol.c $(WLDIR)/linux-dmabuf-unstable-v1-protocol.c $(WLDIR)/wayland-drm-protocol.c
WL_H = $(WLDIR)/xdg-shell-server-protocol.h $(WLDIR)/linux-dmabuf-unstable-v1-server-protocol.h $(WLDIR)/wayland-drm-server-protocol.h
WL_O = $(WL_C:.c=.o)
override CXXFLAGS += -I$(WLDIR)

$(WLDIR)/xdg-shell-protocol.c: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	@mkdir -p $(WLDIR)
	$(WAYLAND_SCANNER) private-code $< $@
$(WLDIR)/xdg-shell-server-protocol.h: $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
	@mkdir -p $(WLDIR)
	$(WAYLAND_SCANNER) server-header $< $@
$(WLDIR)/linux-dmabuf-unstable-v1-protocol.c: $(WAYLAND_PROTOCOLS)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
	@mkdir -p $(WLDIR)
	$(WAYLAND_SCANNER) private-code $< $@
$(WLDIR)/linux-dmabuf-unstable-v1-server-protocol.h: $(WAYLAND_PROTOCOLS)/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
	@mkdir -p $(WLDIR)
	$(WAYLAND_SCANNER) server-header $< $@
$(WLDIR)/wayland-drm-protocol.c: src/wayland/wayland-drm.xml
	@mkdir -p $(WLDIR)
	$(WAYLAND_SCANNER) private-code $< $@
$(WLDIR)/wayland-drm-server-protocol.h: src/wayland/wayland-drm.xml
	@mkdir -p $(WLDIR)
	$(WAYLAND_SCANNER) server-header $< $@

# the generated protocol code is C: compile it through the C frontend of the same cross compiler (as C++ its const
# interface tables get internal linkage, and a separate CC would be the host's in the Buildroot package build)
$(WLDIR)/%.o: $(WLDIR)/%.c
	$(CXX) -x c $(shell $(PKG_CONFIG) --cflags wayland-server) -O2 -std=c11 -Wall -c $< -o $@

banana-ui: $(SRC) $(WL_O) $(WL_H) $(wildcard src/*.h src/wayland/*.xml rnds/*.h)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRC) $(WL_O) $(LDLIBS)

# box art, title screens, genres and years from libretro's free collections (Settings and the first-time setup)
banana-scrape: src/scrape.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< -lpthread -ldl

host:
	$(MAKE) ALSA=0 banana-ui

install:
	install -D -m 755 banana-ui $(DESTDIR)/usr/bin/banana-ui
	install -D -m 755 banana-scrape $(DESTDIR)/usr/bin/banana-scrape
	# (the folders' contents: "cp -r dir dst" into a dst that is there from a build before nests a copy in it, and
	# the files in use were never replaced)
	mkdir -p $(DESTDIR)/usr/share/banana/rnds $(DESTDIR)/usr/share/banana/splash $(DESTDIR)/usr/share/banana/banana $(DESTDIR)/usr/share/banana/lang
	cp -r assets/lang/. $(DESTDIR)/usr/share/banana/lang/
	cp -r assets/rnds/. $(DESTDIR)/usr/share/banana/rnds/
	cp -r assets/splash/. $(DESTDIR)/usr/share/banana/splash/
	cp -r assets/banana/. $(DESTDIR)/usr/share/banana/banana/	# (fonts/: Inter, the layout themes' "inter" faces)

clean:
	rm -f banana-ui banana-scrape
	rm -rf build

.PHONY: host install clean
