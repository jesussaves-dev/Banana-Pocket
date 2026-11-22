#!/bin/sh
# Pack the built PokeDaisy into a Store tarball: tools/pack-pokedaisy.sh <version> [outdir]
# Needs the package built into output/retroid-pocket-classic (it is off in the shipped defconfig; see docs/POKEDAISY.md).
set -e
cd "$(dirname "$0")/.."
V=${1:?usage: $0 <version> [outdir]}; OUT=${2:-output/store}
O=output/retroid-pocket-classic
SRC=$(ls -d "$O"/build/pokedaisy-*/ | head -n1)
[ -x "$O/target/usr/bin/pokedaisy-linux" ] || { echo "pokedaisy is not built: it is not in the image any more. Add BR2_PACKAGE_POKEDAISY=y to a scratch copy of the defconfig, build with make pokedaisy-retroid-pocket-classic, then rerun (docs/POKEDAISY.md)" >&2; exit 1; }
S=$(mktemp -d); trap 'rm -rf "$S"' EXIT
D=$S/app; mkdir -p "$D/lib" "$OUT"
cp "$O/target/usr/bin/pokedaisy-linux" "$D/"
cp "$O"/target/usr/lib/pokedaisy/*.so "$D/lib/"
cp app/pokedaisy/PokeDaisy.app "$SRC/LICENSE" "$SRC/NOTICE" "$D/"
cp app/pokedaisy/run-pokedaisy "$D/"
sed -e 's#"run": *"[^"]*"#"run": "/data/apps/pokedaisy/run-pokedaisy %ROM%"#' \
    -e "s#\"id\": \"pokedaisy\",#\"id\": \"pokedaisy\",\n        \"version\": \"$V\",#" app/pokedaisy/app.json > "$D/app.json"
T=$OUT/pokedaisy-$V-aarch64.tar
tar --owner=0 --group=0 --numeric-owner --sort=name -cf "$T" -C "$D" .
echo "$T"; tar -tvf "$T"
