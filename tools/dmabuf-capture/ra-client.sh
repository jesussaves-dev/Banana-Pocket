#!/bin/sh
# Test client for tools/dmabuf-capture: direct RetroArch (no cage) in Vulkan or glcore mode.
# Device paths (the RPC's PSX/Dreamcast ROMs); this is a test helper, not part of the image.
#   ra-client.sh vk|gl
mode=${1:-vk}
case $mode in
vk) drv=vulkan; ctx=vk_wayland; core=swanstation; rom="/data/roms/psx/Crash Bandicoot (USA).chd" ;;
gl) drv=glcore; ctx=wayland; core=flycast; rom=$(find /data/roms/dreamcast -maxdepth 1 -type f | sort | head -n 1) ;;
*) echo "usage: $0 vk|gl" >&2; exit 1 ;;
esac
cfg=/tmp/ra-$mode.cfg
printf "%s\n" \
 "video_driver = \"$drv\"" \
 "video_context_driver = \"$ctx\"" \
 "video_monitor_index = \"0\"" \
 "video_fullscreen = \"true\"" \
 "video_rotation = \"0\"" \
 "screen_orientation = \"0\"" \
 "video_threaded = \"false\"" \
 "video_vsync = \"true\"" \
 "audio_enable = \"false\"" \
 "input_driver = \"udev\"" > "$cfg"
export TU_DEBUG=noconform
exec env HOME=/data/retroarch XDG_CONFIG_HOME=/data/retroarch /usr/bin/retroarch -v \
  --config /etc/banana/retroarch.cfg --appendconfig "$cfg" --log-file "/data/banana/logs/ra-$mode-run.log" \
  -L "/usr/lib/libretro/${core}_libretro.so" "$rom" > "/data/banana/logs/ra-$mode.log" 2>&1
