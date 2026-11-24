# libretro.h is the unmodified MIT header from RetroArch v1.22.2.
# https://github.com/libretro/RetroArch/blob/v1.22.2/libretro-common/include/libretro.h
# SHA-256: bd3398d29c3763d18617087020ff56b5450a48a63123403a4b674f8e79947acb

CC ?= cc
CFLAGS ?= -O2
OUT ?= .
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS ?= $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
XDG_XML = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
NATIVE_XML = protocol/banana-retro-v1.xml
WL_HEADERS = $(OUT)/xdg-shell-client-protocol.h $(OUT)/banana-retro-client-protocol.h
WL_CODE = $(OUT)/xdg-shell-protocol.c $(OUT)/banana-retro-protocol.c
VIDEO = src/video_wayland.c src/video_wayland.h include/libretro.h $(WL_HEADERS) $(WL_CODE)
INPUT = src/input_evdev.c src/input_evdev.h
VIDEO_CFLAGS = -Iinclude -I$(OUT) $(shell $(PKG_CONFIG) --cflags wayland-client)
VIDEO_LIBS = $(shell $(PKG_CONFIG) --libs wayland-client)
ALSA_CFLAGS = $(shell $(PKG_CONFIG) --cflags alsa)
ALSA_LIBS = $(shell $(PKG_CONFIG) --libs alsa)
CHECK_SERVER_PKGS = wayland-server libdrm xkbcommon
CHECK_SERVER_HEADERS = $(addprefix $(OUT)/,xdg-shell-server-protocol.h banana-retro-server-protocol.h \
	linux-dmabuf-unstable-v1-server-protocol.h wayland-drm-server-protocol.h viewporter-server-protocol.h)
CHECK_VIDEO_OBJS = $(OUT)/video_wayland.o $(WL_CODE:.c=.o)

strict_cflags = -std=c11 -Wall -Wextra -Werror -pedantic

.PHONY: all check check-binaries check-video check-input check-audio check-saves
all: $(OUT)/banana-retro

$(OUT)/banana-retro: src/main.c src/core.c src/core.h src/audio_alsa.c src/audio_alsa.h src/saves.c src/saves.h $(INPUT) $(VIDEO)
	$(CC) $(CPPFLAGS) $(VIDEO_CFLAGS) $(ALSA_CFLAGS) $(CFLAGS) $(strict_cflags) $(LDFLAGS) -o $@ \
		src/main.c src/core.c src/input_evdev.c src/audio_alsa.c src/saves.c src/video_wayland.c $(WL_CODE) \
		-Wl,--wrap=wl_display_connect -ldl $(VIDEO_LIBS) $(ALSA_LIBS) -pthread $(LDLIBS)

$(OUT)/xdg-shell-client-protocol.h: $(XDG_XML)
	$(WAYLAND_SCANNER) client-header $< $@
$(OUT)/xdg-shell-protocol.c: $(XDG_XML)
	$(WAYLAND_SCANNER) private-code $< $@
$(OUT)/banana-retro-client-protocol.h: $(NATIVE_XML)
	$(WAYLAND_SCANNER) client-header $< $@
$(OUT)/banana-retro-protocol.c: $(NATIVE_XML)
	$(WAYLAND_SCANNER) private-code $< $@

$(OUT)/fixture.so: tests/fixture_core.c include/libretro.h
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) -shared -fPIC $(LDFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/fixture-wrong-api.so: tests/fixture_core.c include/libretro.h
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) -DFIXTURE_WRONG_API -shared -fPIC $(LDFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/fixture-missing-export.so: tests/fixture_core.c include/libretro.h
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) -DFIXTURE_MISSING_EXPORT -shared -fPIC $(LDFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/fixture-runtime: src/main.c src/core.c src/core.h src/audio_alsa.c src/audio_alsa.h src/saves.c src/saves.h $(INPUT) $(VIDEO)
	$(CC) $(CPPFLAGS) $(VIDEO_CFLAGS) $(ALSA_CFLAGS) $(CFLAGS) $(strict_cflags) -DSAVES_TESTING $(LDFLAGS) -o $@ \
		src/main.c src/core.c src/input_evdev.c src/audio_alsa.c src/saves.c src/video_wayland.c $(WL_CODE) \
		-Wl,--wrap=wl_display_connect -ldl $(VIDEO_LIBS) $(ALSA_LIBS) -pthread $(LDLIBS)

$(OUT)/fixture-no-serialize.so: tests/fixture_core.c include/libretro.h
	@printf '%s\n' '{ global: *; local: retro_serialize_size; retro_serialize; retro_unserialize; };' > $(OUT)/hide-serialize.map
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) -shared -fPIC $(LDFLAGS) -Wl,--version-script=$(OUT)/hide-serialize.map -o $@ $< $(LDLIBS)

$(OUT)/fixture-no-serialize-size.so: tests/fixture_core.c include/libretro.h
	@printf '%s\n' '{ global: *; local: retro_serialize_size; };' > $(OUT)/hide-serialize-size.map
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) -shared -fPIC $(LDFLAGS) -Wl,--version-script=$(OUT)/hide-serialize-size.map -o $@ $< $(LDLIBS)

$(OUT)/fixture-no-serialize-call.so: tests/fixture_core.c include/libretro.h
	@printf '%s\n' '{ global: *; local: retro_serialize; };' > $(OUT)/hide-serialize-call.map
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) -shared -fPIC $(LDFLAGS) -Wl,--version-script=$(OUT)/hide-serialize-call.map -o $@ $< $(LDLIBS)

$(OUT)/fixture-no-unserialize.so: tests/fixture_core.c include/libretro.h
	@printf '%s\n' '{ global: *; local: retro_unserialize; };' > $(OUT)/hide-unserialize.map
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) -shared -fPIC $(LDFLAGS) -Wl,--version-script=$(OUT)/hide-unserialize.map -o $@ $< $(LDLIBS)

check-binaries: $(OUT)/banana-retro $(OUT)/fixture.so $(OUT)/fixture-wrong-api.so $(OUT)/fixture-missing-export.so

$(CHECK_SERVER_HEADERS): $(XDG_XML) $(NATIVE_XML)
	$(MAKE) -s -C ../../ui WLDIR="$(abspath $(OUT))" "$(abspath $@)"

$(OUT)/video_wayland.o: src/video_wayland.c src/video_wayland.h src/core.h include/libretro.h $(WL_HEADERS)
	$(CC) $(CPPFLAGS) $(VIDEO_CFLAGS) $(CFLAGS) $(strict_cflags) -c $< -o $@

$(OUT)/%.o: $(OUT)/%.c
	$(CC) $(CPPFLAGS) $(VIDEO_CFLAGS) $(CFLAGS) $(strict_cflags) -c $< -o $@

$(OUT)/fixture-check-video: tests/check-video.c $(CHECK_VIDEO_OBJS) $(CHECK_SERVER_HEADERS) \
		../../ui/src/compositor.cpp ../../ui/src/client_buffer.h ../../ui/src/presenter.h
	$(CXX) $(CPPFLAGS) $(VIDEO_CFLAGS) $(CXXFLAGS) $(shell $(PKG_CONFIG) --cflags $(CHECK_SERVER_PKGS)) \
		-std=c++17 -UNDEBUG -ffunction-sections -fdata-sections -x c++ $< -x none $(CHECK_VIDEO_OBJS) \
		$(LDFLAGS) -Wl,--gc-sections -o $@ $(VIDEO_LIBS) $(shell $(PKG_CONFIG) --libs $(CHECK_SERVER_PKGS))

check-video: $(OUT)/fixture-check-video
	$(OUT)/fixture-check-video

$(OUT)/fixture-check-input: tests/check-input.c src/input_evdev.c src/input_evdev.h include/libretro.h
	$(CC) $(CPPFLAGS) -Iinclude $(CFLAGS) $(strict_cflags) $(LDFLAGS) \
		-Wl,--wrap=open -Wl,--wrap=ioctl -o $@ tests/check-input.c src/input_evdev.c $(LDLIBS)

check-input: $(OUT)/fixture-check-input
	$(OUT)/fixture-check-input

$(OUT)/fixture-check-audio: tests/check-audio.c src/audio_alsa.c src/audio_alsa.h
	$(CC) $(CPPFLAGS) $(ALSA_CFLAGS) $(CFLAGS) $(strict_cflags) $(LDFLAGS) -o $@ \
		tests/check-audio.c src/audio_alsa.c $(ALSA_LIBS) -pthread $(LDLIBS)

check-audio: $(OUT)/fixture-check-audio
	$(OUT)/fixture-check-audio

$(OUT)/saves-test.o: src/saves.c src/saves.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(strict_cflags) -DSAVES_TESTING -c src/saves.c -o $@

$(OUT)/fixture-check-saves: tests/check-saves.c src/saves.h $(OUT)/saves-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(strict_cflags) -DSAVES_TESTING $(LDFLAGS) -o $@ tests/check-saves.c $(OUT)/saves-test.o $(LDLIBS)

check-saves: $(OUT)/fixture-check-saves $(OUT)/fixture-runtime $(OUT)/fixture.so $(OUT)/fixture-no-serialize.so $(OUT)/fixture-no-serialize-size.so $(OUT)/fixture-no-serialize-call.so $(OUT)/fixture-no-unserialize.so
	$(OUT)/fixture-check-saves $(OUT)/fixture-runtime $(OUT)/fixture.so $(OUT)/fixture-no-serialize.so $(OUT)/fixture-no-serialize-size.so $(OUT)/fixture-no-serialize-call.so $(OUT)/fixture-no-unserialize.so

check:
	@set -eu; \
	work=$$(mktemp -d "$${TMPDIR:-/tmp}/banana-retro-check.XXXXXX"); \
	trap 'rm -rf -- "$$work"' 0; \
	trap 'exit 1' HUP INT TERM; \
	$(MAKE) -s OUT="$$work" check-binaries; \
	sh tests/check-core.sh "$$work"; \
	$(MAKE) -s OUT="$$work" check-video; \
	$(MAKE) -s OUT="$$work" check-input; \
	$(MAKE) -s OUT="$$work" check-audio; \
	$(MAKE) -s OUT="$$work" check-saves
