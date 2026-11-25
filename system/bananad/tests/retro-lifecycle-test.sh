#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
HELPER="$ROOT/system/rootfs/usr/libexec/banana/stop-native-session"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

${CC:-cc} ${CFLAGS:--O2} -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
	-I"$HERE/include" -o "$WORK/bananad" "$HERE/bananad.c" -lasound -lm
"$WORK/bananad" --selftest retro-lifecycle
${CC:-cc} ${CFLAGS:--O2} -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
	-I"$HERE/include" -o "$WORK/check-checkpoint" "$HERE/tests/check-checkpoint.c" -lasound -lm
"$WORK/check-checkpoint"
sh -n "$ROOT/system/rootfs/etc/init.d/rcK"
sh -n "$HELPER"

python3 "$HERE/tests/check-rcK-native-child.py" "$HELPER" "$WORK"

echo "retro-lifecycle-test: passed"
