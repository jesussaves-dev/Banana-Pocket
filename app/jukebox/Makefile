# jukebox. Cross builds set CXX and PKG_CONFIG for the target's sysroot (tools/pack-jukebox.sh does both).
#   make                 the player (needs libsdl2, SDL2_ttf, SDL2_image, libavformat, libavcodec, libswresample)
#   make selftest        host-only: render every screen to PNGs and check playback/seek/advance (needs ffmpeg)
CXX ?= g++
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O2
override CXXFLAGS += -std=c++17 -Wall -Wextra -Werror
override LDLIBS += -lpthread
PKGS = sdl2 SDL2_ttf SDL2_image libavformat libavcodec libswresample libavutil
override CXXFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
override LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

jukebox: main.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ main.cpp $(LDLIBS)

selftest: jukebox
	sh tests/run.sh

clean:
	rm -f jukebox

.PHONY: selftest clean
