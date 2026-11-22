#!/bin/sh
# Print one index.json entry for a release tarball: tools/store-entry.sh <tarball> <url> <minBanana> <version>
# The tarball holds the app folder's contents (app.json at its top). Paste the output into the index.json of the Banana-Pocket-Apps repo.
set -e
[ $# -eq 4 ] || { echo "usage: $0 <tarball> <url> <minBanana> <version>" >&2; exit 2; }
t=$1
man=$(tar -xOf "$t" ./app.json 2>/dev/null || tar -xOf "$t" app.json)
j() { printf '%s\n' "$man" | python3 -I -c 'import sys,re,json
s=re.sub(r"^\s*//.*$","",sys.stdin.read(),flags=re.M)
print(json.loads(s).get(sys.argv[1],""))' "$1"; }
printf '{"id":"%s","name":"%s","version":"%s","description":"","size":%s,"minBanana":"%s","url":"%s","sha256":"%s"}\n' \
    "$(j id)" "$(j name)" "$4" "$(wc -c < "$t")" "$3" "$2" "$(sha256sum "$t" | cut -d' ' -f1)"
