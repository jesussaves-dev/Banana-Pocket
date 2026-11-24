#!/bin/sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CORE_DIR=${1:?usage: generate-saves.sh CORE_DIR OUTPUT_DIR}
OUTPUT_DIR=${2:?usage: generate-saves.sh CORE_DIR OUTPUT_DIR}
RETROARCH=${RETROARCH:-retroarch}
ROM_DIR=${ROM_DIR:-$OUTPUT_DIR/roms}

python3 "$HERE/generate-roms.py" "$ROM_DIR"
mkdir -p "$OUTPUT_DIR"
for core in mgba gambatte fceumm; do
	case $core in
		mgba) rom=$ROM_DIR/mgba/mgba.gba; library=mGBA; sram=131072; rtc=0 ;;
		gambatte) rom=$ROM_DIR/gambatte/gambatte.gb; library=Gambatte; sram=32768; rtc=8 ;;
		fceumm) rom=$ROM_DIR/fceumm/fceumm.nes; library=FCEUmm; sram=8192; rtc=0 ;;
	esac
	save_dir=$OUTPUT_DIR/$core
	mkdir -p "$save_dir"
	config=$OUTPUT_DIR/retroarch-$core.cfg
	cat > "$config" <<EOF
video_driver = "null"
audio_driver = "null"
input_driver = "null"
savefile_directory = "$save_dir"
EOF
	"$RETROARCH" --verbose --config "$config" -L "$CORE_DIR/${core}_libretro.so" \
		--max-frames=20 "$rom" > "$OUTPUT_DIR/$core.log" 2>&1
	stem=$(basename "${rom%.*}")
	[ "$(wc -c < "$save_dir/$library/$stem.srm")" -eq "$sram" ] || {
		echo "$core produced unexpected SRAM size" >&2; exit 1;
	}
	if [ "$rtc" -gt 0 ]; then
		[ "$(wc -c < "$save_dir/$library/$stem.rtc")" -eq "$rtc" ] || {
			echo "$core produced unexpected RTC size" >&2; exit 1;
		}
	fi
done
find "$OUTPUT_DIR"/mgba "$OUTPUT_DIR"/gambatte "$OUTPUT_DIR"/fceumm \
	-type f -print | sort | xargs sha256sum
