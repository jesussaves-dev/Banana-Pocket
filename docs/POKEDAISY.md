# PokeDaisy (GB / GBC / GBA frontend)

Based on [PokeDaisy](https://github.com/lidor30/pokedaisy) by lidor30, GPL-3.0. Banana ships the unmodified upstream
Linux host and two of its cores; the Banana side is only the package, the manifest and a launcher.

**Status: runs on the handheld (2026-11-18).** Launch from the Apps shelf, input (A/B and X/Y swapped in its SDL map,
F left to Banana's quick menu) and audio work, at 60 fps with `POCKET_NO_VSYNC=1`. The quick menu's `menu_keys` rows are
built but not yet checked on the unit. The image's SDL2 has no dummy video driver, so there is no headless run.

## Where it lives

PokeDaisy is a Store app (apps repo, `pokedaisy`), no longer in the image: the defconfig leaves
`BR2_PACKAGE_POKEDAISY` off. The package stays in `buildroot/package/pokedaisy/` (pins upstream at `79bd00e`) so the
Store tarball can be built from it. An install lives in `/data/apps/pokedaisy/`:

| | |
|---|---|
| `pokedaisy-linux` | The SDL2 host. Needs only SDL2, libm and libc. |
| `lib/pokedaisy_gbc_libretro.so`, `lib/pokedaisy_gba_libretro.so` | GB / GBC core (mGBA) and GBA core (armGBA). |
| `app.json`, `PokeDaisy.app`, `LICENSE`, `NOTICE` | Manifest, marker and upstream's licence files. |
| `run-pokedaisy` | The launcher (source: `app/pokedaisy/run-pokedaisy`). |

To pack a release: add `BR2_PACKAGE_POKEDAISY=y` to a scratch copy of the defconfig (do not commit it), build with
`make pokedaisy-retroid-pocket-classic`, then `tools/pack-pokedaisy.sh <version>` and `tools/store-entry.sh`.

The DS core is not built.

## Build

Upstream pins its emulators as git submodules. The package fetches only the ones this build needs (mgba, armgba,
zlib) as tarballs at the commits upstream pins, via `POKEDAISY_EXTRA_DOWNLOADS`, and unpacks them into
`third_party/` in a post-extract hook. The unused submodules (azahar, melonds and others) never download.
`pokedaisy.hash` holds sha256 for all four tarballs and for upstream's `LICENSE` and `NOTICE`; regenerate it with
every version bump.

```
make pokedaisy-retroid-pocket-classic     # the package alone (needs the symbol on, see above)
```

Bumping upstream means updating `POKEDAISY_VERSION` and the three submodule SHAs together; read them from upstream's
`.gitmodules` / tree at the new commit.

## On the menu

Like BananaTube (docs/BANANATUBE.md), it is an app, not a line in `systems.conf`: `banana-ui` scans
`/data/apps` and shows a tile. `app.json` sets `perf: balanced` and accent `#ff7aa8`. The tile uses the
default app icon until an override is added (docs/THEMES.md, docs/ICONS.md).

## Launch

`run-pokedaisy` follows `run-bananatube`: a Wayland client of `banana-ui`'s compositor (`WAYLAND_DISPLAY=banana-0`,
`XDG_RUNTIME_DIR=/run/banana/wayland`), SDL video `wayland`, SDL audio `alsa` (PipeWire's `default`), and the
`/etc/banana/sdl-pad.map` controller mapping. It sets:

- `POCKET_CORE_DIR=/data/apps/pokedaisy/lib`
- `POCKET_FULLSCREEN=1`
- `POCKET_ROM_DIRS=/data/roms/gba:/data/roms/gbc:/data/roms/gb`, the card folders of the `gba`, `gbc` and `gb` systems (a system's folder defaults to its id under `/data/roms`).

Save data and settings live in `/data/banana/pokedaisy` (`--data`); `HOME` is `/data/banana/pokedaisy/home`, as the
root filesystem is read-only. The log is `/data/banana/logs/pokedaisy.log`, with
the previous run in `.1`. The script runs the host in its own process group, forwards `TERM`/`INT`/`HUP` to it, and
kills any stragglers on exit, so sleep (`SIGSTOP`/`SIGCONT`) and quitting behave as in docs/EMULATORS.md.

## Quick menu

F opens Banana's quick menu over PokeDaisy like over any app (`banana-ui` freezes it with `SIGSTOP`). `app.json`'s
`menu_keys` (`"Save state=F2,Load state=F4"`) adds those rows: choosing one thaws the game and `banana-ui` taps the key in
PokeDaisy through the compositor's keyboard forwarding (`PauseMenu::keyQueue`, sent by the main loop once the client has
the keys back). Load state asks for a second A, as RetroArch's does. PokeDaisy keeps one state per game, so there is no
slot picker. Fast-forward (R2) and rewind (L2) are hold keys in PokeDaisy, so they are not rows; `run-pokedaisy` sets them once with no modifier and rewind on (its own default needs SELECT held and has rewind off). PokeDaisy's own menu is
START + SELECT. `menu_keys` is generic: any app manifest can use it (names: F1-F12, SPACE, ENTER, TAB, ESC, A-Z).

## Hardware notes

- The RPC Classic has no analog sticks. Anything upstream binds to a stick needs a button alternative; upstream's
  `SELECT` + D-pad stylus control (DS) is not relevant until the DS core exists.
- The panel is portrait behind the compositor's landscape picture; check the window's orientation on hardware.

## To do

- Boot, tile, launch, input and audio on hardware; a performance profile.
- Tile icon.
- System shaders and console borders (frames): both live in `banana-ui`'s compositor, not in PokeDaisy's code, so they
  could be offered for PokeDaisy too -- the Shaders settings (mask, colour) probably already draw over it, and a
  Frame row in the quick menu would need the app marked framable (`gameFramable`). Check what applies and what
  looks right on a 160x144 / 240x160 picture.
- DS core (`libretro/linux/libretro-ds`, needs the `dsperate-lr` submodule fetched recursively).
- Tell the upstream author before distributing builds that include it.

## License

PokeDaisy is GPL-3.0 (`POKEDAISY_LICENSE = GPL-3.0+` in the package; upstream's `LICENSE` and `NOTICE` are the
license files). The bundled cores carry their own licenses (mGBA and armGBA ship MPL-2.0 texts; zlib its own `LICENSE`). Only
upstream's `LICENSE` and `NOTICE` are in `POKEDAISY_LICENSE_FILES`, so a `make legal-info` report lacks those three;
add them before a release.
