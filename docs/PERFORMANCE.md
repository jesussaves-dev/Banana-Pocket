# Performance baseline

A host-side measurement of `banana-ui`, so a change can be told from a regression. It is **not** the handheld's
numbers: it runs on a PC, headless, with no panel and no GPU. What it is good for is (a) the *ratio* between the
640×480 and 1024×768 canvases and (b) catching a change that makes the menu slower. The handheld's own costs are measured there, not here:
the unit's numbers live in `docs/ROADMAP.md` (the sleep drain) and in the logs `tools/qemu-boot.sh` writes.

## Running it

    make bench                 # 600 games, 3 runs, both canvas sizes
    make bench BENCH_GAMES=200 # or: ui/tests/bench.sh 200 1

`ui/tests/bench.sh` builds a synthetic library (sparse ROMs, no art), then runs banana-ui headless with the `boot`
and `bench` script commands. The middle run is reported (the first is cold). Every line is from banana-ui's log:

- **scan** — `Library::scan()`: read every shelf's folder, its `gamelist.xml`, Banana's play stats and favourites.
- **menu** — the menu's constructor (the theme's build) plus the first settle: the layout of the home shelf and
  the pictures' decode.
- **first-frame** — the first `Render()`, the pixels the panel sees first: the rasterizing of boxes and glyphs.
- **bench** — N frames 16.7 ms apart; `average`/`worst` are the main thread's time per frame, the drawing alone,
  and the picture worker's; then how many of the screens were unchanged and how many pixels were drawn again.

## Baseline (2026-10, this repo's `main`)

A Ryzen host, `-O2`, one thread, 600 games across the shelves. Idle = the shelf at rest; scrolling = a `right`
press every 10 frames.

| Canvas | scan | menu | first-frame | idle frame | scrolling frame | pixels redrawn (scroll) |
|---|---|---|---|---|---|---|
| 640×480 | ~1.6 ms | ~3.3 ms | ~9.5 ms | ~0.07 ms | ~0.97 ms avg / 1.9 ms worst | ~41% |
| 1024×768 | ~1.8 ms | ~10 ms | ~45 ms | ~0.07 ms | ~2.8 ms avg / 6.5 ms worst | ~49% |

**Re-measured 2026-11-18** (`pocket.json` layout theme, 640×557, the canvas the handheld draws): scan ~1.0 ms, menu
~1.5 ms, first frame ~1.4 ms, idle ~0.15 ms (299 of 300 unchanged), scrolling ~0.20 ms avg / 0.65 ms worst with ~2.4%
of pixels redrawn. Not comparable row for row with the table above (that was the old carousel at 640×480 and
1024×768): the layout theme redraws only the selection that moved. Between 2026-11 and this date `bench.sh` gave no
theme to the menu, so it drew nothing and reported ~0 everywhere; it now fails loudly instead.

What the numbers say:

- **The scan is cheap, even at 600 games** (~2 ms): it is directory reads plus two small text files per shelf, not
  a decode. It scales with the *number of files*, not their size. It is not a boot bottleneck on this library size;
  a card with tens of thousands of ROMs (or a large `gamelist.xml`) is the case to re-measure.
- **The first frame is the expensive part** (~10 ms at 640, ~45 ms at 768): the glyph and box rasterization, and it
  scales with the canvas area (2.56× more pixels ≈ 4.7× the time, so it is not linear — the raster glyph cache
  is per-size and rebuilt at the larger scale). This is why the boot animation is played *while* the menu is built
  and the first frame is drawn under the iris, not on the panel: `irisToMenu()` in `src/main.cpp`.
- **Idle frames cost nothing** (~0.07 ms, 598 of 600 screens unchanged): the draw-list diff holds, as
  `docs/ARCHITECTURE.md` claims (the "well under a millisecond on a PC").
- **Scrolling is 1–3 ms**: ~41–49% of pixels redrawn — the list tiles and the shelf that moved, not the whole
  screen. This is the number to watch for I-1 (SIMD `soft.cpp`) and I-4 (`kms.cpp` rotation).

## What a change moved (why this doc exists)

The compositor's cost is **opaque blits through `memcpy`**, which is already SIMD and bandwidth-bound; the two
compute loops that were *not* vectorized are the mixed-run blend in `soft.cpp` and the rotated present in
`kms.cpp`. Profiling a 1024×768 scroll (gprof, the `bench` workload) put `blitRuns` at 46% of the menu's own time,
and within it:

| loop | share of a scrolling frame | after |
|---|---|---|
| mixed-run blend (`soft.cpp`, `s + mul255(d, 255-sa)`) | ~11% | 1.54× faster (a 16-bit-lane SIMD `blendOver`) |
| rotated present (`kms.cpp` `turn`, the device's path) | not measurable on the host | 1.58× faster (a 4×4 register transpose) |

The mixed blend and `turnRect` are **byte-exact** with the scalar loops: `turnRect` is checked by
`ui/tests/turn-test.cpp` (run by `make test`) for every rectangle of the panel's own size and odd ones, and the
blend against 16M random inputs. The end-to-end scrolling frame moved from ~2.8 ms to ~2.5 ms at 1024×768 (~10%);
the rotation is only exercised on the handheld (QEMU's panel reports no orientation), so its win shows on the
device, not here.

> **Verifying pixel-exactness across a rebuild:** the renderer is *not* byte-reproducible between two binaries of
> different layout — GCC contracts some of the rasterizer's float math into FMA at `-O2`, and the pattern of contraction
> shifts a glyph by a pixel. Two builds of the *same* source can render differently for that reason. To compare a
> build against another, build both with `CXXFLAGS="-O2 -ffp-contract=off"`; then the same screens come out
> identical. (`make test` does not compare against golden images, so this only matters when verifying a change.)

## Native and RetroArch runs on the unit

The menu numbers above come from `make bench` on a host. A game's pacing, heat and energy on the handheld are
collected with the matched-run tools in [RPC-TEST.md](RPC-TEST.md#matched-benchmark-runs) and are **not yet
measured**: there are no native-versus-RetroArch figures in this repository, and a host run cannot produce them.

What a run records, and how to read it:

| source | records | read it as |
|---|---|---|
| `tools/rpc-emu-bench.sh` | thermals, fan, per-policy and devfreq clocks, load, memory, emulator CPU share | context for a pacing difference (a throttled cluster explains a slow run); not a score |
| native `Telemetry run:` / frame line (`BANANA_RETRO_TELEMETRY=1`) | frame statistics of one process, bound by pid, start time and run id | pacing of the native runtime only; RetroArch has no such line |
| `tools/rpc-power-sample.sh` | battery `avg_mW`, `energy_mWh`, per-sample flags, the power source (`RPC_POWER_SRC=vi\|gauge`) | discharge power while `Discharging` and no charger; plausibility-checked, units not detected; `vi` is derived from voltage x current and unqualified against a meter |
| `tools/rpc-run-join.sh` | the pair's windows | whether the two logs are one valid run |

Compare only runs of the same game, core, ROM, settings, brightness, volume and starting battery and temperature,
with the same window length, and repeat each configuration (heat and charge state drift between runs). The bench
header records the core and ROM sha256 and the process's executable sha256 so a comparison can show it used the same
files. Report the range of the repeats, never a single run, and say that the power figure is a whole-device battery
draw, not the emulator's share.

The pair's windows are integer seconds, so a boundary is known to +-1 s, and the join bounds how much longer the power
window may be than the bench's (`RPC_JOIN_TOL`, `RPC_JOIN_MAX_EXTRA`; see RPC-TEST.md). The native frame line covers the
process lifetime, not the bench window, so it is comparable only between native runs of equal play time. `energy_mWh`
belongs to the power window, and is not corrected for the margins.

Power sources differ: on the unit the `power_now` gauge was seen frozen near 127 W against about 1.1-1.7 W from voltage
x current, so choose the source explicitly (`RPC_POWER_SRC`), compare only logs of the same source
(`rpc-run-join.sh --compare-power`), and treat the charge-counter energy as coarse supporting evidence (RPC-TEST.md).

## Re-baselining

Re-run `make bench` after a change that touches `soft.cpp`, `kms.cpp` or `library.cpp`, and update this table when
the numbers move on purpose (a faster path) rather than by accident (a regression). CI does not assert these
numbers (a shared runner is too noisy); they are for a person to read.

## A very large library (2026-10)

The scan's cost was measured again with a synthetic card of **57,000 games and a 30,000-entry `gamelist.xml`**
(`make bench` covers only 600):

| library | scan |
|---|---|
| 600 games | ~2 ms |
| 57,000 games, no `gamelist.xml` | ~200 ms |
| 57,000 games + a 30,000-entry `gamelist.xml` | ~390 ms |

So the scan is a one-time cost that stays in the low milliseconds for any ordinary card and only reaches a few
hundred ms on the host (a second or more on the handheld's slower CPU and SD card) for tens of thousands of games
with a scraped `gamelist.xml`. It runs on the loader thread while the boot animation plays, so it is normally
hidden. **It was not worth a cache**: the cost is bounded, one-time, and only large for a library size the handheld
is unlikely to see, while a cached library adds a staleness bug (a copied ROM not showing up) that is worse than
the wait. The file's cost is the per-game work (a dozen `stat`s for art and the `gamelist.xml` parse), which is
what a cache would have to key on.
