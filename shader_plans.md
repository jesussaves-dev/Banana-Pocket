# System-wide CRT and subpixel shaders: implementation plan

## Status (2026-11-11): complete, merged to `main` at `c170e5b`

Branch `feat/system-shaders`, cut from `feat/bananatube-player-and-shelves` (the branch that has the compositor; `main` does not). That settles open decision 2.

| Phase | State | Commit |
|---|---|---|
| 1, pure logic | done: `ui/src/shader_mask.h`, `ui/tests/shader-mask-test.cpp`, wired into `ui/tests/run.sh` | `b2a99b2` |
| 2, zero-heap commit | done: `AtomicBuf` in `kms.h`; `commitFrame` fills it and calls `DRM_IOCTL_MODE_ATOMIC`. `objs[]` fixed to one id per group; `1a33036` wrote it per property, which sent the overlay's properties to the primary | `1a33036`, `a19f45a` |
| 0, hardware gate | **passed**: `drm-plane-probe --color`. CTM on/off without a modeset and plane alpha at 50% and 100% confirmed by eye on the panel; transparent mask pixels stay undimmed | `9f0901e` |
| 3, color | done: atomic `GAMMA_LUT`, `colorRebuild`, `shaderSetColor`, `nightSettle` removed. Saved night light confirmed by eye at first flip and after a wake. A CTM profile through `colorRebuild` confirmed in phase 5 | `cc224a7` |
| 4, mask | done: `mask[2]`, composed base, menu moves to the primary under a mask, `shownLayer` falls through to the mask. Config keys are read at startup via `shaderApply` in `main.cpp`. Confirmed by eye in the menu, after a wake, in a framed game, and under the volume card and pause menu | `2276d04` |
| 5, settings | done: Shaders group (mask, strength, colour), `Item::PERCENT`, `takeShaderChanged`, 4×2 grid with `kGridCols`. All three masks, the strength slider, the three colour profiles, D-pad and touch confirmed on the unit; `pixel` and `pocket-ocean` checked in headless renders. Tile labels are 17 px so "Display & sound" fits | `8245138` |
| 6, game integration | done: `maskGeomFor` in `applyFrame`, MENU + Select chord. Alignment and chord confirmed on the unit in a Game Boy art frame (cell 7 at 60,36). `maskGeomFor` only uses a system's scale when integer scaled; `maskIntegerHole` places a no-frame integer game; psx is left out because its size depends on the core | `1e659b3` |
| 7, acceptance | done, with the known items below | `ff922fd` (docs) |

**Phase 7 results (mask at 70%, Trinitron):** 22 completed wakes in the menu and 21 with a resident game, all back on the same `banana-ui` process with the planes in place, colour and mask confirmed by eye. Pacing after the game wakes: 16.67 ms mean, 33.31 ms worst, 0 drops over about 60 s. Overlay confirmed parked (`crtc=(null)`, `fb=0`) with mask off and no frame. Volume card and pause menu confirmed over the mask, framed and unframed.

**Known items:**
- `EBUSY` bursts on the first commits after a wake, only with a mask or colour active: 45 failures in 21 game wakes, against 2 in 9 with both off and 2 in 4 on the pre-shader build. At most 5 frames (about 83 ms) as the screen returns; not visible, cause not found.
- One `[dpu error] enc35 timeout pending` in about 60 wakes, on the retry after a failed suspend. The suspend failure (`-95`, then the fallback) predates the shader code.

`make -C ui` is clean and `make -s test` passes. Phase 2 pacing on the unit, 2640 frames of a resident Game Boy game: 16.67 ms mean interval, 18.90 ms worst, 0 drops.

Rulings taken while writing phases 1 and 2 — follow them, or change them deliberately:
- `maskAlphaAt` (and `maskMod`) are public, so the rotation test has a reference independent of `maskFill`; the plan's signature list did not name them.
- The LCD cell is square: `gridPeriod` on both axes. `MaskGeom`'s defaults are `rowPeriod = 4, gridPeriod = 5`, so splitting the cell across both fields made the default pattern lopsided.
- CRT at periods other than 3 or 4 keeps one `0xC0` row and one `0x20` row; aperture at other periods repeats the 4-row table. The plan measures only 3 and 4.
- `maskGeomFor` takes gb/gbc/gba/pico8 from the id, then a 320-wide (3x) or 256-wide (4x) integer-scaled hole from `holeW`; anything else is period 4 at the hole origin.
- `kColorCtm` is `inline constexpr` in the header (C++17), not `extern` plus a second file.
- `AtomicBuf::groups` is the ioctl's `count_objs`: the kernel's `drm_atomic_uapi.c` reads one object per group, then `counts[i]` properties through `props`/`vals` in order. This was checked against the kernel source, not against libdrm's encoder.

**Not verified:** the Shaders rows are English only, like the Frames rows. `feat/drm-plane-compositor` doesn't exist, so `feat/bananatube-player-and-shelves` was inspected, which already contains the finished compositor. No 158-screenshot count was found: the repo has 24 goldens, and 158 only appears as a tile height.

**Three parts of the brief don't work as written on this hardware:**
- **Mask plane:** there's no free plane, so the mask has to share overlay 44.
- **R-G-B tint:** plane blending can't tint per channel without lifting black above `#000000`.
- **240p scanlines:** 1080/240 is 4.5 rows, so they need a per-system period.

## Discovery

| Area | Finding | Location |
|---|---|---|
| Planes | Exactly two: VIG0 and DMA0, exposed as primary 38 and overlay 44 | patch `0009`, `sm4450_sspp`; probe report :32-55 |
| Panel | Native 1080×1240. Plane rotate-90 is rejected, so the mask is generated pre-rotated | probe :20, :73-77 |
| Overlay users | Menu, card or game frame, selected by `shownLayer()` | `compositor.cpp:119` |
| Plane lookup | `findPlanes` already reads zpos, alpha and blend. Modeset sets primary 0, overlay `zpos_max` | `kms.cpp:149-200`, `:221-225` |
| Commit | Allocates a request every frame, which breaks the zero-heap rule today | `kms.cpp:517-539` |
| Wake | bananad sends SIGUSR1, then `restore()` does a modeset and legacy gamma, then `comp.resume()` | `main.cpp:2513-2529`, `kms.cpp:384-396` |
| Settings | Groups rather than tabs, capped at 6 tiles. The footer touch band at y 418-466 blocks a third row | `settings.cpp:284`, `:1411`, `:970-976` |
| Input | The UI reads the pad in-game. `EVIOCGRAB` is only held during the pause menu | `input.cpp:123`, `main.cpp:1948-1961` |
| Persistence | `key = value` through `setConfValue`, with defaults in `KEYS[]` | `settings.cpp:54`, `:666`; `settings_defaults.cpp:36` |

**Kernel facts that shape the design (Linux v7.2 source):**
- `CONVERT_S3_15` drops the sign bit, so every CTM coefficient must be non-negative.
- There's no degamma, so the CTM runs on gamma-encoded values.
- `drm_crtc_legacy_gamma_set` sets CTM to NULL, so the current `applyGamma` would wipe the matrix on every wake.
- Re-sending the same blob ID doesn't set `color_mgmt_changed`, so re-applying after resume needs a fresh blob.
- `ctm || gamma_lut` reserves the DSPP. Always attaching a LUT keeps it reserved, so switching CTM to 0 is safe.

## 1. Plane strategy verdict

Use a single multiplexed overlay. A third plane would need another SSPP in the kernel catalog, and it hasn't been established that the hardware has one.

| Mode | Primary | Overlay 44 | Intensity |
|---|---|---|---|
| Mask off | unchanged | unchanged | n/a |
| Menu, mask on | menu buffer | mask | plane alpha |
| Game, no frame or card | game | mask | plane alpha |
| Game with frame | game | frame composited over mask | baked in |
| Game with card or pause menu | game | card over that same base | baked in |

- The slider only exists in the menu, so every slider move is a single-integer update. Baking only happens at game launch, on a card, or on the chord.
- Frames composite above the mask, so the mask only shows through the frame's hole.
- The feature adds no GPU work, but the existing EGL rotate pass stays because plane rotation is rejected.
- The mask isn't free: a full-panel overlay reads about 321 MB/s at 60 Hz.

## 2. Data structures and signatures

New file `ui/src/shader_mask.h`, pure and DRM-free like `turn.h`:

```cpp
enum ShaderMask : uint8_t { SHADER_MASK_OFF, SHADER_MASK_CRT_240P,
    SHADER_MASK_APERTURE_GRILLE, SHADER_MASK_LCD_GRID, SHADER_MASK_COUNT };
enum ColorProfile : uint8_t { COLOR_PROFILE_BYPASS, COLOR_PROFILE_TRINITRON_9300K,
    COLOR_PROFILE_PVM_6500K, COLOR_PROFILE_HANDHELD_LCD, COLOR_PROFILE_COUNT };
struct MaskGeom { int rowPeriod = 4, gridPeriod = 5, originX = 0, originY = 0; };

void maskFill(uint32_t* dst, size_t pitchWords, int W, int H, int rot,
              ShaderMask m, const MaskGeom& g);
void maskCompose(uint32_t* base, const uint32_t* frameOrNull, const uint32_t* mask,
                 size_t words, int pct);
MaskGeom maskGeomFor(const char* systemId, int holeX, int holeY, int holeW, bool integerScaled);
constexpr uint32_t maskPlaneAlpha(int pct) { return ((uint32_t)pct * 65535u + 50u) / 100u; }
void colorLutFill(drm_color_lut* lut, unsigned n, ColorProfile c, int warmth);
extern const uint64_t kColorCtm[COLOR_PROFILE_COUNT][9];
```

Additions to `Display` in `kms.h`:

```cpp
// public; all run outside the flip path, and the next commit carries the result
bool shaderSetMask(ShaderMask m, const MaskGeom& g);  // fills the hidden buffer
void shaderSetIntensity(int pct);                     // stores one integer
bool shaderSetColor(ColorProfile c);                  // builds fresh blobs
uint32_t maskFb() const;                              // 0 when off

// private
struct ColorState { uint32_t ctm = 0, lut = 0, retired[4] = {}; bool dirty = false; };
struct AtomicBuf  { uint32_t objs[3], counts[3], props[32]; uint64_t vals[32]; };
// Panel: Buf mask[2]; int maskFront; uint32_t crtc_ctm, crtc_gamma_lut, lut_size;
//        ColorState color; bool colorSettle;
// Display: AtomicBuf atomicBuf; drm_color_lut lutScratch[1024];
//          std::vector<uint8_t> maskCopy, frameRaw;  // frameCopy becomes the composed base
void colorRebuild(Panel& pn);
```

- **`commitFrame`:** keeps its signature, fills `atomicBuf` and calls `DRM_IOCTL_MODE_ATOMIC` directly. That removes libdrm's allocator from the flip path. Worst case is 28 properties.
- **Overlay alpha:** `overlayFb == maskFb() ? maskPlaneAlpha(pct) : 0xFFFF`, set on every commit.
- **Blob IDs:** added only when `color.dirty`. Old IDs are destroyed in the next `colorRebuild`, never during a commit.
- **Resume:** `restore()` calls `colorRebuild` twice, one set for the modeset and a fresh set for the first flip. This replaces the `nightSettle` workaround at `main.cpp:2200`, `:2398` and `:2407`.
- **Compositor:** `shownLayer()` becomes `overlayFb ? overlayFb : frameFb ? frameFb : d->maskFb()`.

Config keys:

| Key | Values | Default |
|---|---|---|
| `shader_mask` | `off`, `crt240`, `aperture`, `lcd` | `off` |
| `shader_mask_intensity` | `10` to `100` | `50` |
| `shader_color` | `bypass`, `trinitron`, `pvm`, `handheld` | `bypass` |

UI state:
- Add `Item::PERCENT`, reusing the ten-segment renderer without the brightness side effects.
- Add `takeShaderChanged()`, modelled on `takeNightLightChanged()`.
- Append `"Shaders"` after `"Frames"` so the `press r ×4` test path stays valid.
- Change the grid to 4×2, because a third row collides with the footer. Use one `kGridCols` constant for rendering, touch and a corrected Up/Down at `settings.cpp:909`.

Chord: **MODE + Select** is recommended over Select + R1. It's unbound, and it fits the existing "MENU plus button" branch, whereas Select + R1 also reaches the game as normal input. It toggles the mask on and off without saving.

## 3. Math and pixel layout

- **Rotation:** logical `(x, y)` maps to panel row `x`, column `1079 - y`. Logical scanlines are therefore panel columns. Always use the buffer's real pitch.
- **Encoding:** the word is `0xAARRGGBB`, stored in memory as `BB GG RR AA`. Every mask pixel has RGB = 0, so a pixel with alpha `AA` is the bytes `00 00 00 AA`. A prototype confirmed black stays exactly black.

Alpha per logical row, `j = (y - originY) mod period`:

| Mask | j0 | j1 | j2 | j3 | Luminance kept at 100% |
|---|---|---|---|---|---|
| CRT, period 4 | `20` | `00` | `20` | `C0` | 75% |
| CRT, period 3 | `18` | `00` | `C0` | n/a | not computed |

Aperture grille, column `(x - originX) mod 3`. These are luma-coded stripes, keeping 67% at period 4:

| Row | Col R | Col G | Col B |
|---|---|---|---|
| j0 | `3D` | `10` | `6A` |
| j1 | `30` | `00` | `60` |
| j2 | `3D` | `10` | `6A` |
| j3 (gap) | `A5` | `90` | `BA` |

For period 3, the rows are `3A 0C 67`, `30 00 60`, `A5 90 BA`.

LCD grid, period P: the last column or last row of each cell is `A0`, the corner where they cross is `C8`, and the rest is `00`. That keeps 77% at P = 5.

Geometry from `maskGeomFor`. Confirm the system IDs against `sd.id`:

| Content | Scale | Period | Origin (x, y) |
|---|---|---|---|
| GB / GBC | 7× | 7 | 60, 36 |
| GBA | 5× | 5 | 20, 140 |
| Pico-8 | 8× | 8 | 108, 28 |
| 224-line, 256 wide | 4× | 4 | 108, 92 |
| 320×240 | 3× | 3 | 140, 180 |
| Unknown, or Banana frame | none | 4 | hole origin; won't align to source pixels |

Plane alpha for 10% through 100%: `199A 3333 4CCD 6666 8000 9999 B333 CCCC E666 FFFF`. Blend mode stays premultiplied (0).

CTM values are row-major, non-negative, quantized to 2⁻¹⁵ so the kernel's shift is exact, and each row sums to at most 1.0. `COLOR_PROFILE_BYPASS` uses blob ID 0. The 9300K gains come from the CIE white point, assuming sRGB-like primaries. The PVM and Handheld gains are starting choices, so tune all three by eye on the panel.

```c
/* TRINITRON_9300K */
0x00000000C9B60000, 0x0000000004E00000, 0x0000000000F20000,
0x0000000002B00000, 0x00000000DD4E0000, 0x0000000001060000,
0x0000000003100000, 0x0000000006020000, 0x00000000F6EE0000,
/* PVM_6500K */
0x00000000F53C0000, 0x0000000009040000, 0x0000000001C00000,
0x00000000047A0000, 0x00000000F36A0000, 0x0000000001B60000,
0x0000000004460000, 0x0000000008620000, 0x00000000E16C0000,
/* HANDHELD_LCD */
0x00000000BE080000, 0x000000002A600000, 0x00000000083A0000,
0x0000000014AA0000, 0x00000000C9DA0000, 0x0000000007E20000,
0x0000000011EA0000, 0x00000000232A0000, 0x00000000929A0000,
```

The PVM LUT is `round(65535 * (i/1023)^(2.4/2.2))`, multiplied by the night-light gains so both share the one `GAMMA_LUT`. Entry 0 stays 0.

## 4. File-by-file execution checklist

Host gate for each phase: `make -j$(nproc) 2>&1 | tail -n 30`, then `make -s test 2>&1 | grep -iE "error|warning|fail" | tail -n 30`.

1. **Phase 0, hardware gate (`tools/drm-plane-probe.c`).**
   - Look up `CTM` and `GAMMA_LUT`.
   - Commit NULL → CTM → NULL without `ALLOW_MODESET`, and confirm an asymmetric matrix visually.
   - Confirm plane alpha at 50% with a real commit, since the probe only ran `TEST_ONLY`.
   - Run through `tools/drm-probe-window.sh`. **Stop if CTM is missing or needs a modeset.**
2. **Phase 1, pure logic.** Add `ui/src/shader_mask.h`, `ui/tests/shader-mask-test.cpp` and the `ui/Makefile` wiring. Test all four rotations, black preservation, row sums and the low 17 bits being zero.
3. **Phase 2, zero-heap commit (`kms.h`, `kms.cpp`).** Replace `commitFrame`'s request with `AtomicBuf`, with no behaviour change. Re-run a pacing check on the unit.
4. **Phase 3, color (`kms.*`, `main.cpp`).** Look up the CRTC properties, move night light to atomic `GAMMA_LUT`, keep legacy gamma only for non-atomic drivers such as QEMU, add `colorRebuild`, and remove `nightSettle`.
5. **Phase 4, mask (`kms.*`, `compositor.cpp`).**
   - Lazily allocate `mask[2]`, always writing the hidden buffer.
   - Swap the menu to the primary in `present`, `showMenu` and `atomicModeset`.
   - Add the composed base to `frameSet` and `overlayCardBegin`, plus the `shownLayer` change.
6. **Phase 5, settings (`settings.h`, `settings.cpp`, `settings_defaults.cpp`, `system/rootfs/etc/banana/settings.conf`, `main.cpp`).** Add the rows, keys and 4×2 grid, apply saved values next to `nightLight()` at `main.cpp:1500`, and handle changes next to `:2249`. Regenerate only `640x557-settings-top.png`, and check that labels fit 148 px tiles in `pixel` and `pocket-ocean`.
7. **Phase 6, game integration (`main.cpp`).** Call `maskGeomFor` in `applyFrame` (`:1587`), reset geometry in `finishGame`, and add the chord in the `quickPending` branch (`:1958`).
8. **Phase 7, hardware acceptance.**
   - 20 sleep/wake cycles with mask and CTM active, in both the menu and a resident game.
   - Volume card and pause menu over the mask.
   - 60 fps pacing, no `dpu` errors in `dmesg`, and the parked plane confirmed in debugfs.
   - Update `docs/ARCHITECTURE.md`.

## Risks

- A static mask wears AMOLED rows unevenly over long sessions, which could leave faint permanent scanlines. The 50% default reduces that but doesn't remove it.
- A per-game RetroArch `crt` or `lcd` shader will stack with the mask.

## Open decisions

1. **Aperture tint:** the luma-coded stripes above are recommended, as they keep true `#000000`. Real R-G-B tint needs additive color, for example `0x..0C0000`, and that lights black pixels.
2. **Branch:** start from `main` as a new `feat/system-shaders`, or from the current branch? The uncommitted `.dtsi` change is unrelated and should stay out either way. *(Settled: `feat/system-shaders` from `feat/bananatube-player-and-shelves`; the `.dtsi` stayed uncommitted.)*
3. **Test gate:** what does 158/158 refer to? Until that's settled, the gate is `make test` with all 24 goldens.
