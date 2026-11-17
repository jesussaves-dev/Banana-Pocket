#!/bin/sh
# Generate buildroot/board/retroid-pocket-classic/linux-patches/ from banana-linux.
#
# Usage: tools/kernel-patches-gen.sh [--check] <banana-linux dir> [<base> [<tip>]]
#   base defaults to v7.2, tip to HEAD.
#   --check: generate into a temp dir and compare with the committed patch dir;
#            exit 1 if anything differs (CI and pre-commit use this).
#
# All format-patch output knobs are pinned on the command line and user/system
# git config is ignored, so the output depends only on the commits:
#   --zero-commit        "From 000…" instead of the sha (rebases do not churn it)
#   --no-signature       no "-- \n2.x.y" trailer (it varies with the git version)
#   -N                   "[PATCH]", not "[PATCH 07/45]" (inserting a patch only renumbers files)
#   --full-index         full blob ids; abbrev length grows with repo size (shallow vs full clone)
#   --no-renames, --diff-algorithm=myers, --encode-email-headers,
#   --filename-max-length=64   the defaults, spelled out so diff.* / format.* config cannot change them
#   --attr-source=<tip>  .gitattributes (diff=cpp, diff=dts: the hunk headers' function names) from the tip's tree,
#                        so a bare or shallow clone (CI) gives the same output as a worktree
set -eu

check=0
if [ "${1:-}" = "--check" ]; then
	check=1
	shift
fi
[ $# -ge 1 ] || { sed -n '2,8p' "$0"; exit 2; }
src=$1
base=${2:-v7.2}
tip=${3:-HEAD}

top=$(cd "$(dirname "$0")/.." && pwd)
dst=$top/buildroot/board/retroid-pocket-classic/linux-patches

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 \
	git -C "$src" --attr-source="$tip" -c diff.noprefix=false -c diff.mnemonicPrefix=false \
	-c format.subjectPrefix=PATCH \
	format-patch -q -o "$tmp" \
	--zero-commit --no-signature -N --full-index --no-renames \
	--diff-algorithm=myers --encode-email-headers --filename-max-length=64 \
	"$base..$tip"

n=$(find "$tmp" -name '*.patch' | wc -l)
[ "$n" -gt 0 ] || { echo "kernel-patches-gen: no commits in $base..$tip" >&2; exit 1; }

if [ "$check" -eq 1 ]; then
	if diff -r "$dst" "$tmp" >/dev/null; then
		echo "kernel-patches-gen: $n patches, identical to $base..$tip"
		exit 0
	fi
	echo "kernel-patches-gen: linux-patches/ differs from $src $base..$tip:" >&2
	diff -rq "$dst" "$tmp" | sed "s|$tmp|<generated>|" >&2 || true
	exit 1
fi

rm -f "$dst"/*.patch
cp "$tmp"/*.patch "$dst"/
echo "kernel-patches-gen: wrote $n patches to ${dst#"$top"/}"
