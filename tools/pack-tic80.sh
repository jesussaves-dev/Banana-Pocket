#!/bin/sh
# Pack a cross-built TIC-80 into a Store tarball: tools/pack-tic80.sh <version> <build/bin dir> [outdir]
# The dir holds tic80 and the script plugins (lua.so, js.so, ...): build ALL targets (plain `make`), not just `make tic80`.
# Build it with the image toolchain: cmake -DCMAKE_TOOLCHAIN_FILE=output/retroid-pocket-classic/host/share/buildroot/
# toolchainfile.cmake -DCMAKE_C_FLAGS="-O2 -g0" -DCMAKE_CXX_FLAGS="-O2 -g0" (the toolchain's _FILE_OFFSET_BITS flags break
# vendor/zip) -DBUILD_SDL=ON -DBUILD_SOKOL=OFF -DPREFER_SYSTEM_SDL2=ON -DUSE_GLES2=ON -DUSE_EGL=ON -DBUILD_STATIC=OFF
# -DBUILD_PLAYER=OFF -DBUILD_TOUCH_INPUT=OFF -DBUILD_WITH_ALL=OFF -DBUILD_WITH_{JS,MOON,FENNEL,WREN,SQUIRREL,SCHEME,WASM,JANET}=ON,
# from the upstream tag; then `make`. It needs only libs the image has.
set -e
cd "$(dirname "$0")/.."
V=${1:?usage: $0 <version> <build/bin dir> [outdir]}; B=${2:?usage: $0 <version> <build/bin dir> [outdir]}; OUT=${3:-output/store}
[ -f "$B/tic80" ] && [ -f "$B/lua.so" ] || { echo "$B needs tic80 and the plugins (lua.so ...)" >&2; exit 1; }
S=$(mktemp -d); trap 'rm -rf "$S"' EXIT
D=$S/app; mkdir -p "$D" "$OUT"
cp "$B/tic80" "$B"/*.so "$D/"; chmod 755 "$D/tic80"
for f in "$D/tic80" "$D"/*.so; do ${STRIP:-output/retroid-pocket-classic/host/bin/aarch64-linux-strip} "$f" 2>/dev/null || true; done
cp app/tic80/TIC-80.app app/tic80/run-tic80 "$D/"
[ -n "$LICENSE_FILE" ] && cp "$LICENSE_FILE" "$D/LICENSE"
sed -e "s#\"id\": \"tic80\",#\"id\": \"tic80\",\n        \"version\": \"$V\",#" app/tic80/app.json > "$D/app.json"
T=$OUT/tic80-$V-aarch64.tar
tar --owner=0 --group=0 --numeric-owner --sort=name -cf "$T" -C "$D" .
echo "$T"; tar -tvf "$T"
