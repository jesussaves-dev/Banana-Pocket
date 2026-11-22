#!/bin/sh
# Cross-build Jukebox with the image toolchain and pack a Store tarball: tools/pack-jukebox.sh <version> [outdir]
# Needs a built image tree (output/retroid-pocket-classic): it links the target's SDL2, SDL2_ttf, SDL2_image and
# FFmpeg libraries, all of which ship in the image, so the tarball holds just the binary, launcher and manifest.
set -e
cd "$(dirname "$0")/.."
V=${1:?usage: $0 <version> [outdir]}; OUT=${2:-output/store}
O=output/retroid-pocket-classic; H=$O/host/bin
[ -x "$H/aarch64-linux-g++" ] || { echo "no toolchain at $H (build the image first)" >&2; exit 1; }
make -C app/jukebox clean >/dev/null
make -C app/jukebox CXX="$PWD/$H/aarch64-linux-g++" PKG_CONFIG="$PWD/$H/pkg-config" CXXFLAGS="-O2 -g0"
S=$(mktemp -d); trap 'rm -rf "$S"; make -C app/jukebox clean >/dev/null' EXIT
D=$S/app; mkdir -p "$D" "$OUT"
cp app/jukebox/jukebox "$D/"; chmod 755 "$D/jukebox"
$H/aarch64-linux-strip "$D/jukebox" 2>/dev/null || true
cp app/jukebox/Jukebox.app app/jukebox/run-jukebox "$D/"
[ -n "$LICENSE_FILE" ] && cp "$LICENSE_FILE" "$D/LICENSE"
sed -e "s#\"id\": \"jukebox\",#\"id\": \"jukebox\",\n        \"version\": \"$V\",#" app/jukebox/app.json > "$D/app.json"
for l in $(readelf -d "$D/jukebox" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
    case $l in libc.so*|libm.so*|libstdc++*|libgcc_s*|libpthread*|ld-linux*) continue;; esac
    [ -e "$O/target/usr/lib/$l" ] || [ -e "$O/target/lib/$l" ] || { echo "image lacks $l" >&2; exit 1; }
done
T=$OUT/jukebox-$V-aarch64.tar
tar --owner=0 --group=0 --numeric-owner --sort=name -cf "$T" -C "$D" .
echo "$T"; tar -tvf "$T"
