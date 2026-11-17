#!/bin/sh
# Put a freshly built kernel into an existing Banana card image: slot a's Image on the boot partition, and its
# /lib/modules inside slot a's squashfs. Everything else (the userspace, the boot config) stays as it was, so a
# released image plus this kernel is what the kernel CI job boots with tools/qemu-boot.sh: a kernel-only change is
# tested without building the whole system.
#
#   tools/kernel-into-image.sh <banana-*.img[.gz]> <Image> <lib/modules dir> <out .img>
#
# <lib/modules dir> is the target's lib/modules (one <kernel.release> dir in it, as Buildroot installs them).
# Needs mtools, squashfs-tools, sfdisk, python3 and fakeroot (the squashfs is unpacked and repacked in one fakeroot
# session, so its owners and modes survive without root). SQUASHFS_BIN=<dir> picks the squashfs tools (e.g. a
# Buildroot output's host/bin).
set -eu
[ $# -eq 4 ] || { sed -n '6p' "$0" >&2; exit 2; }
IMG=$1 KIMAGE=$2 MODS=$3 OUT=$4
[ -f "$IMG" ] && [ -f "$KIMAGE" ] && [ -d "$MODS" ] || { echo "kernel-into-image: missing input" >&2; exit 2; }
n=$(find "$MODS" -mindepth 1 -maxdepth 1 -type d | wc -l)
[ "$n" -eq 1 ] || { echo "kernel-into-image: $MODS holds $n kernel releases, want 1" >&2; exit 1; }
REL=$(basename "$(find "$MODS" -mindepth 1 -maxdepth 1 -type d)")
SQ=${SQUASHFS_BIN:+$SQUASHFS_BIN/}

case $IMG in *.gz) gunzip -c "$IMG" > "$OUT" ;; *) cp "$IMG" "$OUT" ;; esac
part() { sfdisk -J "$OUT" | python3 -c "import json,sys; p=json.load(sys.stdin)['partitiontable']['partitions'][$1-1]; print(p['start']*512, p['size']*512)"; }
read -r BOOT_AT _ <<EOF
$(part 1)
EOF
read -r ROOT_AT ROOT_SIZE <<EOF
$(part 2)
EOF

# slot a must be the one the card boots (qemu-boot.sh boots extlinux.conf's DEFAULT and its update test copies slot a)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
mcopy -o -i "$OUT@@$BOOT_AT" ::/extlinux/extlinux.conf "$tmp/extlinux.conf"
def=$(sed -n 's/^DEFAULT *//p' "$tmp/extlinux.conf" | head -n 1)
lin=$(awk -v l="$def" '$1 == "LABEL" { cur = $2 } $1 == "LINUX" && cur == l { print $2 }' "$tmp/extlinux.conf")
case $lin in /a/Image) ;; *) echo "kernel-into-image: DEFAULT $def boots $lin, not /a/Image" >&2; exit 1 ;; esac

mcopy -o -i "$OUT@@$BOOT_AT" "$KIMAGE" ::/a/Image

# slot a's squashfs: its length from the superblock (bytes_used, 8 bytes at offset 40), compressor from unsquashfs -s
used=$(python3 -c "import struct,sys; f=open(sys.argv[1],'rb'); f.seek(int(sys.argv[2])+40); print(struct.unpack('<Q', f.read(8))[0])" "$OUT" "$ROOT_AT")
dd if="$OUT" of="$tmp/root.sqfs" bs=1M iflag=skip_bytes,count_bytes skip="$ROOT_AT" count="$used" status=none
comp=$("${SQ}unsquashfs" -s "$tmp/root.sqfs" | sed -n 's/^Compression //p')
old=$(fakeroot -- sh -eu -c '
	sq=$1 tmp=$2 mods=$3 rel=$4 comp=$5
	"${sq}unsquashfs" -q -n -d "$tmp/root" "$tmp/root.sqfs" >/dev/null
	find "$tmp/root/lib/modules" -mindepth 1 -maxdepth 1 -printf "%f " 2>/dev/null || true
	rm -rf "$tmp/root/lib/modules"
	mkdir -p "$tmp/root/lib/modules"
	cp -a "$mods/$rel" "$tmp/root/lib/modules/"
	chown -R 0:0 "$tmp/root/lib/modules"
	"${sq}mksquashfs" "$tmp/root" "$tmp/new.sqfs" -comp "$comp" -noappend -no-progress -quiet >/dev/null
	rm -rf "$tmp/root"
' sh "$SQ" "$tmp" "$MODS" "$REL" "$comp")
size=$(stat -c %s "$tmp/new.sqfs")
[ "$size" -le "$ROOT_SIZE" ] || { echo "kernel-into-image: root $size bytes > partition $ROOT_SIZE" >&2; exit 1; }
dd if="$tmp/new.sqfs" of="$OUT" bs=1M seek="$ROOT_AT" oflag=seek_bytes conv=notrunc status=none
echo "kernel-into-image: slot a now $(basename "$KIMAGE") + modules $REL (was ${old:-none}), root $size bytes"
