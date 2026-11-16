#!/bin/sh
# Push an update package to the handheld over its own USB gadget and, with --apply, write the other slot and restart
# into it. The handheld's port is a USB device that runs an Ethernet (NCM) gadget with a DHCP server, so plugging it
# into this computer gives this end an address with nothing to configure (the handheld is 169.254.170.2, this end
# 169.254.170.1) -- no Wi-Fi needed. ssh has to be on: Settings > Network; the key is the one in
# banana/ssh/authorized_keys on the card, the same the Wi-Fi link uses.
#
#   tools/rpc-push-update.sh                the newest banana-<board>-*.update.tar -> banana/updates on the card
#   tools/rpc-push-update.sh <package.tar>  a chosen package (tar or the unpacked .update)
#   tools/rpc-push-update.sh --apply ...    also run `update apply` (writes the inactive slot) and restart into it
#
# Host: $BANANA_HOST, else root@169.254.170.2 (the USB gadget). $SSH_OPTS overrides the ssh options. The package is
# streamed over ssh (ssh + cat, not scp) so this works with the handheld's dropbear, and read back with its sha256
# before anything is applied. Nothing runs the update until --apply or the menu's Settings > System > Update.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
BOARD=${BOARD:-retroid-pocket-classic}
OUT=${OUT:-$HERE/output}
IMAGES=$OUT/$BOARD/images
HOST=${BANANA_HOST:-root@169.254.170.2}
DEST=/data/banana/updates
UPDATE=/usr/libexec/banana/update
SSH_OPTS=${SSH_OPTS:-"-o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ServerAliveInterval=15"}

APPLY=
P=
for a in "$@"; do
    case "$a" in
    --apply) APPLY=1 ;;
    -*) echo "unknown option $a" >&2; exit 2 ;;
    *) P=$a ;;
    esac
done
[ -n "$P" ] || P=$(ls -t "$IMAGES"/banana-"$BOARD"-*.update.tar 2>/dev/null | head -n 1)
[ -n "$P" ] && [ -f "$P" ] || { echo "no update package found (build one with: make $BOARD), or name one" >&2; exit 1; }
NAME=$(basename "$P")

run() { ssh $SSH_OPTS "$HOST" "$@"; }

echo "== $HOST"
if ! run true 2>/dev/null; then
    echo "no link. Plug the handheld into this computer with a data cable (not a charge-only one), turn ssh on" >&2
    echo "(Settings > Network), and re-seat the cable if nothing enumerated (a usb0/enx... interface should appear)." >&2
    echo "Over Wi-Fi instead: BANANA_HOST=root@<the handheld's address> $0" >&2
    exit 1
fi
echo "== pushing $NAME ($(( $(wc -c < "$P") / 1048576 )) MB) to $DEST"
run "mkdir -p $DEST"
run "cat > $DEST/$NAME.new" < "$P"
run "mv -f $DEST/$NAME.new $DEST/$NAME && sync"
l=$(sha256sum "$P" | cut -d' ' -f1)
r=$(run "sha256sum $DEST/$NAME" | cut -d' ' -f1)
[ "$l" = "$r" ] || { echo "the package on the card is not the one sent (sha256 differs)" >&2; exit 1; }
echo "   sha256 $l"

if [ -n "$APPLY" ]; then
    echo "== applying (writes the inactive slot and restarts into it)"
    run "$UPDATE apply $DEST/$NAME"
    # A reboot sent right after the apply has been seen to do nothing (rc 0, the same uptime; a second one worked).
    # So it is checked: the kernel's boot id must change, else the reboot is sent again; then the handheld is waited
    # for and its version compared with the package's.
    want=$(tar -tf "$P" 2>/dev/null | grep -E '(^|/)VERSION$' | head -n 1)
    [ -z "$want" ] || want=$(tar -xOf "$P" "$want" 2>/dev/null | head -n 1)
    bootid() { run "cat /proc/sys/kernel/random/boot_id" 2>/dev/null || true; }
    b0=$(bootid)
    try=1
    while :; do
        run "sync; reboot" 2>/dev/null || true
        n=0
        while [ $n -lt 20 ]; do
            sleep 1; n=$((n + 1))
            b=$(bootid)
            [ -z "$b" ] || [ "$b" != "$b0" ] && break
        done
        { [ -z "$b" ] || [ "$b" != "$b0" ]; } && break
        [ $try -ge 3 ] && { echo "the handheld is still up after $try reboots: restart it by hand" >&2; exit 1; }
        echo "   still running the same boot after ${n} s: rebooting again"
        try=$((try + 1))
    done
    echo "== restarting"
    n=0
    while [ $n -lt 180 ]; do
        sleep 2; n=$((n + 2))
        b=$(bootid)
        [ -n "$b" ] && [ "$b" != "$b0" ] && break
    done
    [ -n "$b" ] && [ "$b" != "$b0" ] || { echo "the handheld didn't come back within 3 minutes (no link)" >&2; exit 1; }
    got=$(run "cat /etc/banana/version" 2>/dev/null || true)
    if [ -n "$want" ] && [ "$got" != "$want" ]; then
        echo "back up, but running $got, not $want (the slot switch didn't take?)" >&2; exit 1
    fi
    echo "== up again, running ${got:-an unknown version}"
else
    echo "== done. On the handheld: Settings > System > Update, or:"
    echo "   ssh $HOST '$UPDATE apply $DEST/$NAME && sync && reboot'"
fi
