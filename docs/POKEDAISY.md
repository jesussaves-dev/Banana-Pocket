# PokeDaisy (GB / GBC / GBA frontend)

Based on [PokeDaisy](https://github.com/lidor30/pokedaisy) by lidor30, GPL-3.0. Banana ships the unmodified upstream
Linux host and two of its cores; the Banana side is only the package, the manifest and a launcher.

**Status: builds, not yet tested on hardware.** Boot, the tile on the Apps shelf, launch, input and audio are
unverified. No display is attached to the dev unit's DP, and the image's SDL2 has no dummy video driver, so there is no
headless run either.

## What is in the image

| | |
|---|---|
| `buildroot/package/pokedaisy/` | `BR2_PACKAGE_POKEDAISY` (on in the RPC Classic defconfig). Pins upstream at `79bd00e`. |
| `/usr/bin/pokedaisy-linux` | The SDL2 host. Needs only SDL2, libm and libc. |
| `/usr/lib/pokedaisy/pokedaisy_gbc_libretro.so` | GB / GBC core (mGBA). |
| `/usr/lib/pokedaisy/pokedaisy_gba_libretro.so` | GBA core (armGBA). |
| `/usr/share/banana/apps/pokedaisy/` | `app.json`, the `PokeDaisy.app` marker, and upstream's `NOTICE`. |
| `/usr/libexec/banana/run-pokedaisy` | The launcher (source: `system/rootfs/usr/libexec/banana/run-pokedaisy`). |

The DS core is not built.

## Build

Upstream pins its emulators as git submodules. The package fetches only the ones this build needs (mgba, armgba,
zlib) as tarballs at the commits upstream pins, via `POKEDAISY_EXTRA_DOWNLOADS`, and unpacks them into
`third_party/` in a post-extract hook. The unused submodules (azahar, melonds and others) never download.
`pokedaisy.hash` holds sha256 for all four tarballs and for upstream's `LICENSE` and `NOTICE`; regenerate it with
every version bump.

```
make pokedaisy-retroid-pocket-classic     # the package alone
make retroid-pocket-classic               # the image
```

Bumping upstream means updating `POKEDAISY_VERSION` and the three submodule SHAs together; read them from upstream's
`.gitmodules` / tree at the new commit.

## On the menu

Like BananaTube (docs/BANANATUBE.md), it is an app, not a line in `systems.conf`: `banana-ui` scans
`/usr/share/banana/apps` and shows a tile. `app.json` sets `perf: balanced` and accent `#ff7aa8`. The tile uses the
default app icon until an override is added (docs/THEMES.md, docs/ICONS.md).

## Launch

`run-pokedaisy` follows `run-bananatube`: a Wayland client of `banana-ui`'s compositor (`WAYLAND_DISPLAY=banana-0`,
`XDG_RUNTIME_DIR=/run/banana/wayland`), SDL video `wayland`, SDL audio `alsa` (PipeWire's `default`), and the
`/etc/banana/sdl-pad.map` controller mapping. It sets:

- `POCKET_CORE_DIR=/usr/lib/pokedaisy`
- `POCKET_FULLSCREEN=1`
- `POCKET_ROM_DIRS=/data/roms/gba:/data/roms/gbc:/data/roms/gb`, the card folders of the `gba`, `gbc` and `gb` systems (a system's folder defaults to its id under `/data/roms`).

Save data and settings live in `/data/banana/pokedaisy` (`--data`); `HOME` is `/data/banana/pokedaisy/home`, as the
root filesystem is read-only. The log is `/data/banana/logs/pokedaisy.log`, with
the previous run in `.1`. The script runs the host in its own process group, forwards `TERM`/`INT`/`HUP` to it, and
kills any stragglers on exit, so sleep (`SIGSTOP`/`SIGCONT`) and quitting behave as in docs/EMULATORS.md.

## Hardware notes

- The RPC Classic has no analog sticks. Anything upstream binds to a stick needs a button alternative; upstream's
  `SELECT` + D-pad stylus control (DS) is not relevant until the DS core exists.
- The panel is portrait behind the compositor's landscape picture; check the window's orientation on hardware.

## To do

- Boot, tile, launch, input and audio on hardware; a performance profile.
- Tile icon.
- DS core (`libretro/linux/libretro-ds`, needs the `dsperate-lr` submodule fetched recursively).
- Tell the upstream author before distributing builds that include it.

## License

PokeDaisy is GPL-3.0 (`POKEDAISY_LICENSE = GPL-3.0+` in the package; upstream's `LICENSE` and `NOTICE` are the
license files). The bundled cores carry their own licenses (mGBA and armGBA ship MPL-2.0 texts; zlib its own `LICENSE`). Only
upstream's `LICENSE` and `NOTICE` are in `POKEDAISY_LICENSE_FILES`, so a `make legal-info` report lacks those three;
add them before a release.
