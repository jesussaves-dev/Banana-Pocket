#!/bin/sh
# D1 diagnostic build: the Phase 3C multi-buffer client, with the image's own toolchain.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SYS=${SYS:-$ROOT/output/retroid-pocket-classic/host/aarch64-buildroot-linux-gnu/sysroot}
CC=${CC:-$ROOT/output/retroid-pocket-classic/host/bin/aarch64-linux-gcc}
SCAN=${WAYLAND_SCANNER:-$ROOT/output/retroid-pocket-classic/host/bin/wayland-scanner}
XML=${WAYLAND_PROTOCOLS:-/usr/share/wayland-protocols}
B=$HERE/build
mkdir -p "$B"
"$SCAN" client-header "$XML/stable/xdg-shell/xdg-shell.xml" "$B/xdg-shell-client-protocol.h"
"$SCAN" private-code  "$XML/stable/xdg-shell/xdg-shell.xml" "$B/xdg-shell-protocol.c"
"$SCAN" client-header "$XML/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml" "$B/linux-dmabuf-unstable-v1-client-protocol.h"
"$SCAN" private-code  "$XML/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml" "$B/linux-dmabuf-unstable-v1-protocol.c"
CFLAGS="-std=c11 -O2 -Wall -Wextra -Wno-unused-parameter --sysroot=$SYS -I$SYS/usr/include -I$SYS/usr/include/libdrm -I$B"
LDFLAGS="-L$SYS/usr/lib"
$CC $CFLAGS "$HERE/client.c" "$B/xdg-shell-protocol.c" "$B/linux-dmabuf-unstable-v1-protocol.c" \
	$LDFLAGS -lwayland-client -lEGL -lGLESv2 -lgbm -ldrm -o "$B/egl-buffer-client"
ls -l "$B/egl-buffer-client"
