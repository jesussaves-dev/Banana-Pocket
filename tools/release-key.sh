#!/bin/sh
# Release signing key (ed25519). The private key stays on the build machine, never in the repo; the public half goes in
# the image (system/rootfs/etc/banana/release.pub), and `update` refuses a package whose sha256sums.sig doesn't verify.
#   release-key.sh gen             make the key if there is none, write the public half into the rootfs overlay
#   release-key.sh sign <file>     write <file>.sig
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
KEY=${BANANA_SIGN_KEY:-$HOME/.config/banana/release-ed25519.pem}
PUB=$HERE/../system/rootfs/etc/banana/release.pub
case "$1" in
gen)
    if [ -f "$KEY" ]; then echo "key exists: $KEY"; else
        mkdir -p "$(dirname "$KEY")"; ( umask 077; openssl genpkey -algorithm ed25519 -out "$KEY" ) || exit 1
        echo "new key: $KEY -- back it up; a lost key means every installed image has to be reflashed"
    fi
    openssl pkey -in "$KEY" -pubout -out "$PUB" || exit 1
    echo "public key: $PUB" ;;
sign)
    [ -f "$KEY" ] || { echo "release-key.sh: no key at $KEY (run: $0 gen)" >&2; exit 1; }
    openssl pkeyutl -sign -rawin -inkey "$KEY" -in "$2" -out "$2.sig" ;;
*) sed -n 2,6p "$0"; exit 1 ;;
esac
