# Shared installed-core selection for RetroArch and the experimental host.
# Source this file; call banana_resolve_core <configured-core> <system> <game-core>.
banana_resolve_core() {
    requested=$1 system_id=$2 game_core=$3
    libretro_dir=${BANANA_LIBRETRO_DIR:-/usr/lib/libretro}
    settings_file=${BANANA_SETTINGS_FILE:-/data/banana/settings.conf}
    image_settings=${BANANA_IMAGE_SETTINGS_FILE:-/etc/banana/settings.conf}
    banana_core_name=$requested
    banana_core_path=$libretro_dir/${requested}_libretro.so

    banana_setting() {
        value=$(sed -n "s/^ *$1 *= *//p" "$settings_file" 2>/dev/null | tail -n 1)
        [ -n "$value" ] || value=$(sed -n "s/^ *$1 *= *//p" "$image_settings" 2>/dev/null | tail -n 1)
        printf '%s' "$value"
    }

    if [ -n "$system_id" ]; then
        candidate=$(banana_setting "core.$system_id")
        case $candidate in *[!a-zA-Z0-9_]*|'') candidate= ;; esac
        if [ -n "$candidate" ] && [ -f "$libretro_dir/${candidate}_libretro.so" ]; then
            banana_core_name=$candidate
            banana_core_path=$libretro_dir/${candidate}_libretro.so
        fi
    fi
    case $game_core in *[!a-zA-Z0-9_]*|'') game_core= ;; esac
    if [ -n "$game_core" ] && [ -f "$libretro_dir/${game_core}_libretro.so" ]; then
        banana_core_name=$game_core
        banana_core_path=$libretro_dir/${game_core}_libretro.so
    fi
    if [ "$system_id" = psx ] && [ ! -f "$banana_core_path" ] &&
       [ -f "$libretro_dir/pcsx_rearmed_libretro.so" ]; then
        banana_core_name=pcsx_rearmed
        banana_core_path=$libretro_dir/pcsx_rearmed_libretro.so
    fi
    [ -f "$banana_core_path" ] || return 1
    export banana_core_name banana_core_path
}

# Only explicitly validated software profiles are eligible. Runtime selection is
# an opt-in setting in /data/banana/retro/runtime.conf, independent of the
# configured default core; for example: runtime.gba.mgba = banana-retro.
banana_native_eligible() {
    system_id=$1 core_name=$2 rom_path=$3
    case "$system_id$core_name" in *:*) return 1 ;; esac
    # Extensions follow systems.conf: a GBC system also lists .gb; no archives.
    case "$system_id:$core_name:$rom_path" in
        gba:mgba:*.gba|gba:mgba:*.GBA) ;;
        gb:gambatte:*.gb|gb:gambatte:*.GB) ;;
        gbc:gambatte:*.gbc|gbc:gambatte:*.GBC|gbc:gambatte:*.gb|gbc:gambatte:*.GB) ;;
        nes:fceumm:*.nes|nes:fceumm:*.NES) ;;
        *) return 1 ;;
    esac
    [ -f "$rom_path" ] || return 1
    [ -z "${BANANA_SHADER-}" ] || return 1
    [ -z "$(/usr/libexec/banana/ra-secrets get ra_user 2>/dev/null)" ] || return 1
    shader_cfg=${BANANA_RETROARCH_CFG:-/data/retroarch/retroarch.cfg}
    ! grep -Eq '^video_shader_enable = "(true|1)"' "$shader_cfg" 2>/dev/null
}

banana_native_opted_in() {
    system_id=$1 core_name=$2
    case "$system_id$core_name" in *[!a-zA-Z0-9_-]*) return 1 ;; esac
    rules=${BANANA_RETRO_RULES_FILE:-/data/banana/retro/runtime.conf}
    [ "$(sed -n "s/^ *runtime\.$system_id\.$core_name *= *//p" "$rules" 2>/dev/null | tail -n 1)" = banana-retro ]
}
