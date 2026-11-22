#!/bin/sh
# Pack a cross-built LÖVE into a Store tarball: tools/pack-love.sh <version> <love build dir> <sysroot dir> [outdir]
# <love build dir> is the autotools tree's src/ (it holds .libs/love and .libs/liblove-*.so); <sysroot dir> holds the
# cross-built LuaJIT and libmodplug in usr/lib, the two libraries the image doesn't ship.
# Build recipe, with the image toolchain H=output/retroid-pocket-classic/host/bin:
#   LuaJIT v2.1:  make HOST_CC=gcc CROSS=$H/aarch64-linux- TARGET_SYS=Linux amalg PREFIX=/usr; make install DESTDIR=<sysroot>
#   libmodplug 0.8.9.2: autoreconf -fi; ./configure --host=aarch64-linux-gnu CC=$H/aarch64-linux-gcc ...; make install DESTDIR=<sysroot>
#   (rm <sysroot>/usr/lib/*.la)
#   LÖVE 11.5 linux-src tarball: PKG_CONFIG_SYSROOT_DIR=<staging> PKG_CONFIG_LIBDIR=<staging>/usr/lib/pkgconfig:<sysroot>/usr/lib/pkgconfig
#   ./configure --host=aarch64-linux-gnu CC=$H/aarch64-linux-gcc CXX=$H/aarch64-linux-g++ --with-lua=luajit
#   --with-luaversion=5.1 --prefix=/usr --disable-gme PKG_CONFIG=$H/pkg-config (CPPFLAGS/CFLAGS/CXXFLAGS -I<sysroot>/usr/include
#   and .../luajit-2.1, LDFLAGS -L<sysroot>/usr/lib); then `make -C src OBJCXX=$H/aarch64-linux-g++ OBJCXXLD=$H/aarch64-linux-g++`.
set -e
cd "$(dirname "$0")/.."
V=${1:?usage: $0 <version> <love build dir> <sysroot dir> [outdir]}; B=${2:?usage}; SR=${3:?usage}; OUT=${4:-output/store}
[ -f "$B/.libs/love" ] || { echo "$B/.libs/love missing" >&2; exit 1; }
LIB=$(ls "$B"/.libs/liblove-*.so 2>/dev/null | head -n1); [ -f "$LIB" ] || { echo "no liblove-*.so in $B/.libs" >&2; exit 1; }
S=$(mktemp -d); trap 'rm -rf "$S"' EXIT
D=$S/app; mkdir -p "$D" "$OUT"
cp "$B/.libs/love" "$D/love"; cp "$LIB" "$D/"
cp -L "$SR"/usr/lib/libluajit-5.1.so.2 "$SR"/usr/lib/libmodplug.so.1 "$D/"
ST=${STRIP:-output/retroid-pocket-classic/host/bin/aarch64-linux-strip}
for f in "$D/love" "$D"/*.so*; do
    patchelf --remove-rpath "$f" 2>/dev/null || true
    $ST "$f" 2>/dev/null || true
done
chmod 755 "$D/love"
cp -r app/love/picker "$D/picker"; cp app/love/LOVE.app app/love/run-love "$D/"
[ -n "$LICENSE_FILE" ] && cp "$LICENSE_FILE" "$D/LICENSE"
sed -e "s#\"id\": \"love\",#\"id\": \"love\",\n        \"version\": \"$V\",#" app/love/app.json > "$D/app.json"
T=$OUT/love-$V-aarch64.tar
tar --owner=0 --group=0 --numeric-owner --sort=name -cf "$T" -C "$D" .
echo "$T"; tar -tvf "$T"
