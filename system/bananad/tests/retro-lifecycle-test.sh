#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

${CC:-cc} ${CFLAGS:--O2} -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
	-o "$WORK/bananad" "$HERE/bananad.c" -lasound -lm
"$WORK/bananad" --selftest retro-lifecycle
sh -n "$ROOT/system/rootfs/etc/init.d/rcK"

grep -q 'BANANA_RETRO_CTL_PREPARE_SLEEP' "$HERE/bananad.c"
grep -q 'BANANA_RETRO_CTL_SLEEP_ABORT' "$HERE/bananad.c"
grep -q 'BANANA_RETRO_CTL_WAKE' "$HERE/bananad.c"
grep -q 'BANANA_RETRO_CTL_SHUTDOWN' "$HERE/bananad.c"
grep -q 'native-session' "$ROOT/system/rootfs/etc/init.d/rcK"
grep -q 'kill -KILL "$N"' "$ROOT/system/rootfs/etc/init.d/rcK"

echo "retro-lifecycle-test: passed"
