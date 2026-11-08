# Screen bezels for Game Boy, Game Boy Color and Game Boy Advance

## Intent

The 1240×1080 panel leaves large black bars around the three integer-scaled handheld systems. SkyWalker541's
[Retroid Pocket Classic Overlays](https://github.com/SkyWalker541/Retroid-Pocket-Classic-Overlays) (CC0) draw the
console around the game, with a 1px pixel grid inside the screen, so the handheld looks like the hardware it is
emulating. One switch in Settings turns the frames on and off; each system gets its own console's art.

## Scope

- **Systems:** `gb` (DMG), `gbc`, `gba` only.
- **One global switch:** `Settings > RetroArch > Screen bezels` → `bezels = on|off` in `settings.conf`, default `on`.
- **Grid is part of the art:** the shipped files are `*_bezel_grid_integer_1px_black.png` — the console frame and the
  1px LCD grid in one PNG.
- **Shaders:** with bezels on, those three systems run with any per-game shader forced off (the grid and a pixel shader
  double up). With bezels off, the per-game shader applies exactly as it does today. Every other system is untouched and
  may use shaders at any time.

## Non-goals

Grid-only/fullscreen-grid and STN/Pixel Pop overlays, the PSX bezel (its window is 3× while Banana's SwanStation draws
4:3 at ~1240×930), DS (the pack has only a fullscreen logo), PICO-8 (not a system this image ships), per-system
switches, picking between bezel styles at runtime, the custom/Pokemon/holographic/fade art.

## Constraint found

Every frame in the pack is cut for one integer multiple — GB/GBC 160×144 at 7× (window 1120×1008), GBA 240×160 at 5×
(1200×800) — and the pack's non-integer art is grid-only, drawn for a picture stretched past integer scale (the DMG/GBC
grids run at a 7.75px pitch across the whole panel, which is a 1240-wide stretch, not what Banana's aspect-preserving
scaler produces).

So the bezel implies the scale: **while bezels are on, `video_scale_integer` is forced true for `gb`/`gbc`/`gba`**, which
is where the emulated picture lands in the window exactly. Bezels off leaves RetroArch's own setting alone, for anyone
who wants a stretched picture and a shader.

## Design

**Assets.** Three PNGs, copied unchanged from the pack into `system/rootfs/usr/share/banana/overlays/`:
`gb.png` (`DMG_1240_bezel_grid_integer_1px_black.png`), `gbc.png` (`GBC_1240_bezel_grid_integer_1px_black.png`),
`gba.png` (`GBA_1240_bezel_grid_integer_1px_black.png`). `overlays/LICENSE` records CC0 and the source URL; the README
gets the credit.

**Settings.** One row in the existing `RetroArch` section of `ui/src/settings.cpp` over `settings.conf`'s `bezels` key
(`On`/`Off`), plus the commented default in `system/rootfs/etc/banana/settings.conf`. Nothing else in the UI changes.

**Launcher (`run-retroarch`), after the existing shader block:**

1. `bezels` = `setting bezels` (default `on`); bezel system = `gb`, `gbc` or `gba`, else none.
2. Overlay off (switch off, or another system): `set_cfg input_overlay_enable false`. Shader keys are left as the block
   above wrote them, so per-game shaders work as before.
3. Overlay on: `set_cfg video_scale_integer true`, write `/data/retroarch/.banana-overlay.cfg`
   (`overlays = 1`, `overlay0_overlay = <absolute PNG>`, `overlay0_full_screen = true`, `overlay0_descs = 0`),
   then `set_cfg input_overlay <cfg>`, `input_overlay_enable true`, `input_overlay_opacity 1.0`,
   `input_overlay_scale 1.0`, `input_overlay_auto_scale false`, and `video_shader_enable false`.
4. Every launch rewrites those keys, so flipping the switch applies on the next game start with no stale frame or
   disabled shader left behind.

The overlay is display-only (`overlay0_descs = 0`) and 1:1 on a 1240×1080 window: one alpha-blended full-screen quad,
no input changes.

## Testing

- `make -s test`.
- On the handheld: GB, GBC and GBA games with bezels **on** — frame + grid, picture inside the window at 7×/5×.
- Bezels **off** — no frame, and a per-game shader (`lcd`/`crt`/`sharp`) applies again.
- NES/SNES/MD (no bezel) — shader applies with the switch in either position.
- Turning integer scaling off in RetroArch's menu while bezels are on has no effect for the three systems (the launcher
  sets it back); with bezels off it is honoured.

## Risks

- Forcing integer scaling while bezels are on overrides a RetroArch setting the user may have changed. It is the only
  way the pack's windows line up; the Settings help text says so.
- RetroArch screenshots and the achievements HUD draw over the overlay as before; the menu and `bananad` are untouched.
