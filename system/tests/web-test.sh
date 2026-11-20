#!/bin/sh
# banana-web.py on the host: the PIN gate, uploads that stay in their folders, secrets that are never served, the
# cross-site refusal, and a theme-coloured page. A scratch /data, a free port, nothing on the network.
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d); trap 'kill $PID 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p $T/data/banana/logs $T/data/saves $T/run
echo hi > $T/data/banana/logs/boot.txt; echo s > $T/data/saves/a.srm; echo secret > $T/data/banana/wifi.conf
echo "theme = user:street-harbor" > $T/data/banana/settings.conf
# the image's /usr/share/banana: the themes from the rootfs, the fonts from the UI assets (ui's make install)
mkdir -p $T/share; ln -s $HERE/system/rootfs/usr/share/banana/themes $T/share/themes; ln -s $HERE/ui/assets/banana $T/share/banana
export BANANA_DATA=$T/data BANANA_RUN=$T/run BANANA_SHARE=$T/share BANANA_ETC=$HERE/system/rootfs/etc/banana
PORT=$((20000 + $$ % 20000)); U=127.0.0.1:$PORT
python3 -I $HERE/system/rootfs/usr/libexec/banana/banana-web.py --port $PORT >$T/log 2>&1 & PID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s $T/run/web.pin ] && break; sleep 0.3; done
PIN=$(cat $T/run/web.pin); fail=0
cc() { curl -s -c $T/cj -b $T/cj "$@"; }
check() { if [ "$2" = "$3" ]; then :; else echo "FAIL: $1 (got '$2', want '$3')"; fail=1; fi; }
check "PIN asked first" "$(cc $U/ | grep -c 'PIN')" 1
check "wrong PIN" "$(cc -d pin=0 $U/login | grep -c Wrong)" 1
check "locked out before login" "$(cc -o /dev/null -w "%{http_code}" -X POST --data-binary x "$U/upload/bios?name=a")" 401
check "login" "$(cc -o /dev/null -w "%{http_code}" -d pin=$PIN $U/login)" 303
check "home" "$(cc $U/ | grep -c Games)" 1
check "theme accent" "$(cc $U/style.css | grep -c -- '--accent:#52c5ef')" 1
check "font served" "$(cc -o /dev/null -w "%{http_code}" $U/font/BananaPixel.ttf)" 200
check "rom upload" "$(cc -X POST --data-binary @$HERE/Makefile "$U/upload/roms/gba?name=t.gba")" ok
[ -f $T/data/roms/gba/t.gba ] || { echo "FAIL: rom not in roms/gba"; fail=1; }
check "unknown system" "$(cc -o /dev/null -w "%{http_code}" -X POST --data-binary x "$U/upload/roms/zzz?name=a")" 400
check "traversal name" "$(cc -o /dev/null -w "%{http_code}" -X POST --data-binary x "$U/upload/bios?name=..%2Fwifi.conf")" 400
check "secret name" "$(cc -o /dev/null -w "%{http_code}" -X POST --data-binary x "$U/upload/bios?name=ra.token")" 400
check "log read" "$(cc $U/logs/boot.txt | grep -c hi)" 1
check "log traversal" "$(cc -o /dev/null -w "%{http_code}" "$U/logs/..%2Fwifi.conf")" 404
check "saves zip" "$(cc $U/download/saves.zip -o $T/s.zip; python3 -I -c "import zipfile,sys;print(zipfile.ZipFile(sys.argv[1]).namelist())" $T/s.zip)" "['saves/a.srm']"
check "cross-site refused" "$(cc -H 'Origin: http://evil' -o /dev/null -w "%{http_code}" -X POST --data-binary x "$U/upload/bios?name=a")" 403
for i in 1 2 3 4 5 6; do cc -c $T/cj2 -b $T/cj2 -o /dev/null -d pin=0 $U/login; done
check "rate limit" "$(curl -s -o /dev/null -w "%{http_code}" -d pin=$PIN $U/login)" 429
[ $fail = 0 ] && echo "web-test: ok"
exit $fail
