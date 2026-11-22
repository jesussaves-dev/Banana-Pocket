#!/bin/sh
# banana-store against a mock index (file:// URLs): refresh and its offline fallback, install (hash, size, id, minBanana,
# unsafe paths and links all refused, nothing left half-installed), update keeps appdata, remove keeps or wipes data.
cd "$(dirname "$0")/../.." || exit 1
make -s -C ui ALSA=0 banana-store 2>&1 | tail -n 35
S=$PWD/ui/banana-store
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fails=0
ok() { echo "ok   $1"; }; bad() { echo "FAIL $1"; fails=$((fails+1)); }
export BANANA_STORE_ALLOW_FILE=1 BANANA_APPS=$T/apps BANANA_APPDATA=$T/appdata BANANA_CACHE=$T/cache BANANA_RUN=$T/run
export BANANA_VERSION_FILE=$T/version BANANA_STORE_INDEX=file://$T/index.json
mkdir -p "$T/run" "$T/apps"; echo 1.0.1 > "$T/version"
st() { cat "$T/run/store.status"; }
mkapp() {   # mkapp <name> <id> [extra app.json member]: <name>.tar from a folder with a manifest
    mkdir -p "$T/src/$1"; printf '{"id":"%s","name":"X","run":"/bin/true","ext":"app"%s}\n' "$2" "$3" > "$T/src/$1/app.json"
    echo "$1" > "$T/src/$1/payload"; tar -cf "$T/$1.tar" -C "$T/src/$1" .
}
entry() {   # entry <name> <id> <min> [sha override]
    sha=${4:-$(sha256sum "$T/$1.tar" | cut -d' ' -f1)}
    printf '{"id":"%s","name":"X","version":"1.0","size":%s,"minBanana":"%s","url":"file://%s/%s.tar","sha256":"%s"}' \
        "$2" "$(wc -c < "$T/$1.tar")" "$3" "$T" "$1" "$sha"
}
index() { printf '{"version":1,"apps":[%s]}\n' "$*" > "$T/index.json"; }

mkapp good good; mkapp new good ',"v":2'; mkapp other other; mkapp future future ',"minBanana":"9.0"'
mkdir -p "$T/src/evil"; echo '{"id":"evil"}' > "$T/src/evil/app.json"; tar -cf "$T/evil.tar" -C "$T/src/evil" . --transform 's,^\./app.json,../escape,'
mkdir -p "$T/src/lnk"; echo '{"id":"lnk"}' > "$T/src/lnk/app.json"; ln -sf /etc/passwd "$T/src/lnk/x"; tar -cf "$T/lnk.tar" -C "$T/src/lnk" .
index "$(entry good good 1.0),$(entry other other 1.0),$(entry future future 9.0),$(entry good wrongsha 1.0 $(printf '0%.0s' $(seq 64))),$(entry other swapped 1.0),$(entry evil evil 1.0),$(entry lnk lnk 1.0)"

"$S" refresh; [ "$(st)" = done ] && [ -f "$T/cache/store-index.json" ] && ok "refresh caches the index" || bad "refresh ($(st))"
mv "$T/index.json" "$T/index.off"; "$S" refresh; [ "$(st)" = "done offline" ] && ok "offline falls back to the cache" || bad "offline ($(st))"
echo '{"version":1,"apps":[{"id":"../x"}]}' > "$T/index.json"; "$S" refresh; [ "$(st)" = "done offline" ] && cmp -s "$T/cache/store-index.json" "$T/index.off" && ok "an invalid index never replaces the cache" || bad "invalid index ($(st))"
mv "$T/index.off" "$T/index.json"

"$S" install good; [ "$(st)" = done ] && [ "$(cat "$T/apps/good/payload")" = good ] && [ -d "$T/appdata/good" ] && ok "install" || bad "install ($(st))"
echo save > "$T/appdata/good/save.dat"
for c in "wrongsha:hash" "swapped:another app" "future:needs Banana" "evil:unsafe path" "lnk:unsafe link" "nope:not in the store"; do
    id=${c%%:*}; want=${c#*:}
    "$S" install "$id" 2>/dev/null; if st | grep -q "^error.*$want" && [ ! -e "$T/apps/$id" ]; then ok "refuses $id ($want)"; else bad "refuses $id: $(st)"; fi
done
[ ! -e "$T/escape" ] && [ ! -e "$T/apps/escape" ] && ok "nothing escaped the staging folder" || bad "escape"
echo 0.9 > "$T/version"; "$S" install other 2>/dev/null; st | grep -q "^error needs Banana 1.0" && [ ! -e "$T/apps/other" ] && ok "refuses an app newer than the OS" || bad "other ($(st))"
echo 1.0 > "$T/version"; "$S" install other; [ "$(st)" = done ] && ok "minBanana equal to the OS installs" || bad "other ($(st))"
echo 1.0.1 > "$T/version"
index "$(entry new good 1.0)"; "$S" refresh; "$S" install good
[ "$(st)" = done ] && [ "$(cat "$T/apps/good/payload")" = new ] && [ "$(cat "$T/appdata/good/save.dat")" = save ] && [ ! -e "$T/apps/.staging/good.old" ] && ok "update replaces the code, keeps appdata" || bad "update ($(st))"
"$S" remove good; [ ! -e "$T/apps/good" ] && [ -f "$T/appdata/good/save.dat" ] && ok "remove keeps the data" || bad "remove"
"$S" install good; "$S" remove good --wipe-data; [ ! -e "$T/appdata/good" ] && ok "remove --wipe-data" || bad "wipe"
"$S" remove nothing 2>/dev/null; st | grep -q "^error" && ok "remove of a missing app errors" || bad "remove missing"
ls "$T/apps/.staging" 2>/dev/null | grep -q . && bad "staging not empty: $(ls "$T/apps/.staging")" || ok "staging left empty"
exit $fails
