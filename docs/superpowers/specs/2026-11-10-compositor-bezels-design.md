# Bezels by the compositor

## Intent

Move screen frames out of RetroArch's overlay and onto the compositor's overlay plane, so any RetroArch system or port
can have one, chosen per game, with no integer-scale lock-in and no clash with a shader. The pause menu gets a *Frame*
row. Replaces the mechanism in `2026-11-08-bezels-design.md`.

## Decisions

| Topic | Decision |
| --- | --- |
| Picture placement | The client is configured at the hole's size and the EGL pass places it in the hole. |
| LCD grid | Stripped from the shipped art. Holes are fully transparent. |
| New art | One procedural Banana frame, drawn from the active theme. |
| Plane | The frame is on the overlay plane. Cards and the pause menu are merged into a copy of it. |
| Selection | `bezels` master switch, then a per-system default, then a per-game override. |
| Frame row | Live, saved as that game's override. |
| Defaults | `gb`, `gbc`, `gba` framed. Everything else is None until chosen. |
| Procedural hole | About 32 px of border on every side, hole fitted to the system's aspect. |
| Integer scale | A launch with a procedural frame runs integer off. Art frames need no override. |
| Burn-in | A frame brightness setting. |
| RetroArch's menu | No special case: it draws inside the hole. |
| Who can be framed | RetroArch games and ports. BananaTube and the PortMaster entry never are. |

## Non-goals

A card folder for user PNGs, more of the SkyWalker541 pack, generated per-console art, pixel shift, an LCD grid (that
belongs to the roadmap's *Filters for every game*), resizing RetroArch while its own menu is open.

## Design

### Frames (`ui/src/frame.{h,cpp}`, new)

A frame is a 1240x1080 logical ARGB image and a hole rectangle.

- **Art frames.** `overlays/{gb,gbc,gba}.png`, decoded with `stb_image`. The hole is the bounding box of the alpha-0
  pixels. Expected: GB and GBC 1120x1008 at (60,36), GBA 1200x800 at (20,140).
- **Banana frame.** Drawn in code from the active theme's colours, so `pixel`, `pocket ocean` and any user theme match
  without shipping one PNG per theme and aspect. The hole is the largest rectangle of the system's aspect inside the
  panel less 32 px per side (1176x1016): 4:3 is 1176x882. Aspect comes from a small table in `frame.cpp` (default 4:3).
  A port uses the whole 1176x1016, since SDL clients take the size they are configured at.
- **Build path.** Decode or draw, multiply by the brightness level, premultiply, turn into the panel's orientation
  with `turn()`. This runs at launch or on a Frame row change, never per frame. The plane cannot rotate (F2), so the
  turn is done here on the CPU.

`tools/strip-frame-grid.py` clears the hole of each shipped PNG to alpha 0, run once. `overlays/LICENSE` and the README
credit change from "unchanged" to "with the grid removed" (the art is CC0).

### Brightness

`frame_brightness` = `100|85|70|50`, default `85`, in Settings > Frames beside *Screen bezels*.

It is baked into the frame's pixels rather than set through the plane's alpha property. Cards and the pause menu share
that plane, so plane alpha would dim them too. Baking has the same zero per-frame cost.

### KMS (`ui/src/kms.{h,cpp}`)

- `Buf frame[2]`: double-buffered so a Frame row change never writes a buffer the plane may be scanning. 5.3 MB each,
  allocated with the other buffers and freed with them.
- `frameBegin()` / `frameCommit()` / `frameFb()`, mirroring the card calls.
- `overlayCardBegin()` copies the front frame into the hidden card buffer instead of clearing it (clears when there is
  no frame). `overlayCardTurn` is unchanged, since it only writes the card's box.

### Compositor (`ui/src/compositor.{h,cpp}`, `rotate.h`, `egl_presenter.cpp`)

- `setFrame(fb, x, y, w, h)` holds the frame's framebuffer and hole. `fb = 0` is full panel. Called before the game is
  forked, so the first configure already has the right size.
- `overlayFb` falls back to the frame: `setOverlayFb(0)` means "show the frame", so `hideCard` needs no change.
- The configure sends the hole's size. A later `setFrame` with a different hole sends a new configure when the game is
  thawed, not while it is SIGSTOPped. RetroArch chooses its own size after the first configure and only obeys a later
  one (measured), so a frame that comes back the wrong size gets the configure once more.
- `fitRotated` gains a target rectangle in place of the whole output. The EGL pass letterboxes any source into the
  hole, so a client that ignores the configure is still placed correctly.
- `forwardTouch` subtracts the hole's origin and scales to the committed size. Touches on the frame are dropped.
- `resume()` zeroes `overlayFb`; after a wake the frame must be put back. `Display::restore` keeps the buffers, so this
  is a re-commit.
- While frozen, a Frame row change re-renders the last presented buffer into the new hole so the result shows at once.
  The EGL path releases its source after the flip, so the compositor keeps one reference to it (`lastShown`).

### Selection (`ui/src/library.{h,cpp}`, `main.cpp`)

- Per-game: a fourth tab-separated column in `pergame.conf`, written only when set. Older lines still load.
  `overrideOf`'s pair becomes a struct, and an entry is erased only when all three fields are empty.
- Per-system: `frame.<system>` in `settings.conf`, like `core.<system>`. Values `auto`, `none`, `art`, `banana`.
  `frame_brightness` is in `settings_defaults.cpp`'s `KEYS`.
- Resolution: `bezels = off` gives none. Otherwise the game's override, then `frame.<system>`, then `art` for
  `gb`/`gbc`/`gba` and `none` for the rest. `art` on a system with no art falls back to `banana`.
- Launch exports `BANANA_FRAME=none|art|banana` for `run-retroarch`.

### UI

- Pause menu: a *Frame* row using `arrows = true`, as *Change disc* does, cycling None / Console (if the system has
  art) / Banana. Added after *Shader* for RetroArch; ports get `{ "Resume", "Frame", "Quit" }`.
- Game options: a *Frame* row beside *Emulator* and *Shader*, with Default following the system.
- Settings > Frames, a new page (ports are not RetroArch): *Screen bezels* with its help text rewritten, *Frame
  brightness*, and a row per RetroArch or port system for its default.

### Launcher (`run-retroarch`)

The overlay block goes: no `banana-overlay.cfg`, no forced aspect, no forced shader off.

- `BANANA_FRAME=banana` appends `video_scale_integer = "false"`. Only that key is read before the game and restored
  after it.
- `gb`/`gbc`/`gba` always append `input_overlay_enable = "false"`, so a config left behind by an older build or a crash
  cannot draw the old frame on top of the new one.

## Testing

Host (`make -s test`):

- `rotation-test`: `fitRotated` with a target rectangle.
- New `frame-test`: hole detection on the three stripped PNGs, the Banana hole per aspect, premultiply and brightness,
  resolution order, three- and four-column overrides.
- Goldens for the pause menu and Game options rows, in both themes.
- `check-systems.py` passes with the new keys.

On the handheld:

- GB, GBC, GBA framed: the picture exactly fills the hole at 7x and 5x, and a Slang shader runs under the frame.
- SNES and a port opted into Banana: picture fills the hole; card's integer value is back after quitting.
- Frame row: None to Console to Banana while paused; picture moves at once and the client resizes on Resume.
- Volume pill, low-battery card and pause menu draw over a frame at full brightness; the frame returns after each.
- `s2idle` sleep and wake with a frame up, and with the pause menu up.
- Touch in a framed port lands on the right spot.
- `3f` log over a few minutes with a frame on and off.

## Risks

- **Scanout cost.** A full-panel overlay held through play is new; so far it has only been up with the pause menu or a
  card. Measure drops with the `3f` log before anything else. If it costs frames, the frame moves into the EGL pass.
- **Resizing RetroArch live.** A Vulkan swapchain resize on thaw is unproven here. Fallback if it misbehaves: the live
  preview stays, and the client keeps its old size until the next launch (the EGL pass letterboxes it into the hole).
- **Integer scale cannot change mid-game.** Choosing Banana from the pause menu while RetroArch has integer on shows a
  smaller picture in the hole until the next launch, since `run-retroarch` sets it at launch.
