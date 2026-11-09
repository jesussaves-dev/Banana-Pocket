#!/bin/sh
# Build the Phase 2 dmabuf capture server and the SDL2 test client with the image's own toolchain.
# Generates the xdg-shell and linux-dmabuf server code into build/ (gitignored).
#
#   tools/dmabuf-capture/build.sh
#   SYS=... CC=... WAYLAND_PROTOCOLS=... tools/dmabuf-capture/build.sh   (overrides)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SYS=${SYS:-$ROOT/output/retroid-pocket-classic/host/aarch64-buildroot-linux-gnu/sysroot}
CC=${CC:-$ROOT/output/retroid-pocket-classic/host/bin/aarch64-linux-gcc}
SCAN=${WAYLAND_SCANNER:-$ROOT/output/retroid-pocket-classic/host/bin/wayland-scanner}
XML=${WAYLAND_PROTOCOLS:-/usr/share/wayland-protocols}
B=$HERE/build
mkdir -p "$B"

"$SCAN" server-header "$XML/stable/xdg-shell/xdg-shell.xml" "$B/xdg-shell-server-protocol.h"
"$SCAN" private-code  "$XML/stable/xdg-shell/xdg-shell.xml" "$B/xdg-shell-protocol.c"
"$SCAN" server-header "$XML/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml" "$B/linux-dmabuf-unstable-v1-server-protocol.h"
"$SCAN" private-code  "$XML/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml" "$B/linux-dmabuf-unstable-v1-protocol.c"
# wl_drm is shipped by Mesa's Wayland EGL (and used by wlroots/cage); the XML lives in the Mesa source
DRM_XML=${WAYLAND_DRM_XML:-/usr/share/wayland-eglstream/wayland-drm.xml}
[ -f "$DRM_XML" ] || DRM_XML=$(ls "$ROOT"/output/retroid-pocket-classic/build/mesa3d-*/src/egl/wayland/wayland-drm/wayland-drm.xml 2>/dev/null | head -n1)
"$SCAN" server-header "$DRM_XML" "$B/wayland-drm-server-protocol.h"
"$SCAN" private-code  "$DRM_XML" "$B/wayland-drm-protocol.c"

CFLAGS="-std=c11 -O2 -Wall -Wextra -Wno-unused-parameter --sysroot=$SYS -I$SYS/usr/include -I$SYS/usr/include/libdrm -I$B"
LDFLAGS="-L$SYS/usr/lib"

$CC $CFLAGS "$HERE/capture-server.c" "$B/xdg-shell-protocol.c" "$B/linux-dmabuf-unstable-v1-protocol.c" "$B/wayland-drm-protocol.c" \
	$LDFLAGS -lwayland-server -ldrm -o "$B/dmabuf-capture"
$CC $CFLAGS -I"$SYS/usr/include/SDL2" "$HERE/sdl2-gl-client.c" \
	$LDFLAGS -lSDL2 -o "$B/sdl2-gl-client"

ls -l "$B/dmabuf-capture" "$B/sdl2-gl-client"
