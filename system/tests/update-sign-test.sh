#!/bin/sh
# update's signature gate, on the host: a package signed by the release key installs past the check, a tampered,
# unsigned or wrongly signed one is refused, and allow-unsigned opts a hand-made one in. Stops at "no partition":
# the check runs before anything is written.
cd "$(dirname "$0")/../.." || exit 1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
UP=system/rootfs/usr/libexec/banana/update
fails=0
ok() { echo "ok   $1"; }; bad() { echo "FAIL $1"; fails=$((fails+1)); }
for k in good evil; do openssl genpkey -algorithm ed25519 -out "$T/$k.pem"; done
openssl pkey -in "$T/good.pem" -pubout -out "$T/good.pub"
mkpkg() {   # mkpkg <dir>
    mkdir -p "$1"; echo img > "$1/Image"; echo dtb > "$1/x.dtb"; echo root > "$1/rootfs.squashfs"
    echo t > "$1/VERSION"; echo b > "$1/board"
    (cd "$1" && sha256sum Image x.dtb rootfs.squashfs VERSION board > sha256sums)
}
run() {   # run <pkg>: the failure line update writes
    BANANA_PUBKEY=$T/good.pub BANANA_ALLOW_UNSIGNED=$T/allow BANANA_RUN=$T/run BANANA_UPDATES=$T/u sh "$UP" apply "$1" 2>&1 | tr '\n' ' '
}
mkdir -p "$T/run"
mkpkg "$T/p1"; openssl pkeyutl -sign -rawin -inkey "$T/good.pem" -in "$T/p1/sha256sums" -out "$T/p1/sha256sums.sig"
run "$T/p1" | grep -q "not signed" && bad "signed package refused" || ok "signed package passes the gate"
mkpkg "$T/p2"
run "$T/p2" | grep -q "not signed" && ok "unsigned refused" || bad "unsigned accepted"
mkpkg "$T/p3"; openssl pkeyutl -sign -rawin -inkey "$T/evil.pem" -in "$T/p3/sha256sums" -out "$T/p3/sha256sums.sig"
run "$T/p3" | grep -q "not signed" && ok "wrong key refused" || bad "wrong key accepted"
cp -r "$T/p1" "$T/p4"; echo evil >> "$T/p4/sha256sums"
run "$T/p4" | grep -q "not signed" && ok "tampered sums refused" || bad "tampered sums accepted"
touch "$T/allow"
run "$T/p2" | grep -q "not signed" && bad "allow-unsigned ignored" || ok "allow-unsigned opts in"
# a signed manifest that leaves a file out is refused
mkpkg "$T/p5"; echo x > "$T/p5/y.dtb"
openssl pkeyutl -sign -rawin -inkey "$T/good.pem" -in "$T/p5/sha256sums" -out "$T/p5/sha256sums.sig"
run "$T/p5" | grep -q "doesn't cover y.dtb" && ok "unlisted dtb refused" || bad "unlisted dtb accepted"
mkpkg "$T/p6"; (cd "$T/p6" && sha256sum Image x.dtb rootfs.squashfs > sha256sums)
openssl pkeyutl -sign -rawin -inkey "$T/good.pem" -in "$T/p6/sha256sums" -out "$T/p6/sha256sums.sig"
run "$T/p6" | grep -q "doesn't cover VERSION" && ok "unlisted VERSION refused" || bad "unlisted VERSION accepted"
exit $fails
