# Banana OSD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Volume and brightness inside a game show the menu's pill (icon, 10-segment bar, percent) and achievements show a Banana-coloured badge card, by patching RetroArch's on-screen drawing.

**Architecture:** One Buildroot patch touches three RetroArch files: `command.c` gives the two bridge tokens a short duration, `gfx/gfx_widgets.c` draws a message starting `BN V ` or `BN B ` as the pill, and `gfx/widgets/gfx_widget_achievement_popup.c` switches its card and text to Banana's colours and drops the English header. `banana-ui` writes the active card's colours to `/data/banana/osd-style`, the patch reads them once, and `bananad` sends numbers instead of sentences.

**Tech Stack:** C99 (RetroArch 1.22.2, patched at build time), C++17 (banana-ui), POSIX shell (launcher), Buildroot, POSIX sh test harness.

**Spec:** `docs/superpowers/specs/2026-11-05-banana-osd-design.md`

## Global Constraints

- No new dependency, no new process, no socket.
- The patch lives in `buildroot/patches/retroarch/` (applied through `BR2_GLOBAL_PATCH_DIR="$(BR2_EXTERNAL_BANANA_PATH)/patches"`), named `0014-…` after `0013-cheevos-tell-the-menu-what-a-game-unlocked.patch`.
- RetroArch's ordinary messages (save state, screenshots, fast-forward) must render exactly as today.
- `bananad` is strict POSIX C99 with `-Wall -Wextra -Werror`; no `sprintf` into fixed buffers without bounds.
- Pill geometry is the menu's (`ui/src/settings.cpp` `Settings::level()`): 310x42 at `y0 = 4` on the 640x557 logical canvas, half-width bar gap `max(2, round(3*s))`, percent 19 px in `0xffe68a`.
- Colours: text `0xf1ede2`, bar `0xffe68a`, unlit `0x3a3540`, mute `0xff8a96`.

## Review Focus

- A message that only *looks* like a token (a core printing `BN V 5`) must not become a pill — the prefix must be exact and the tail must be all digits.
- `/data/banana/osd-style` missing, empty or garbage (an older menu, a hand-edited file) must still draw the pill with the dark defaults.
- An ordinary message arriving while a pill is on screen must not be drawn as a pill or be swallowed.
- Two volume presses in quick succession must update one pill rather than stack two.
- With `hud = 0`, neither the pill nor the achievement card may appear, and the menu's post-game toasts must still fire.

---

### Task 1: The menu publishes the card's colours

**Files:**
- Modify: `ui/src/settings.cpp` (the palette the `osd` box uses, `settings.cpp:1014`)
- Test: `ui/tests/run.sh`

**Interfaces:**
- Produces: `/data/banana/osd-style`, three lines: `bg <hex6>`, `edge <hex6>`, `crisp <0|1>`; `bg`/`edge` are the palette values the `osd` box draws with, lower-case hex without `#`.

- [ ] **Step 1: Write the failing test** — in `ui/tests/run.sh`, after the menu starts headless, assert the file exists and its three lines match the `dark` defaults:

```
bg 211f26
edge f1ede2
crisp 0
```

- [ ] **Step 2: Run it and watch it fail**

Run: `ui/tests/run.sh 2>&1 | tail -n 20`
Expected: FAIL — `/data/banana/osd-style` does not exist. The harness must launch the menu with `BANANA_DATA=<scratch>` (the same override `main.cpp` already honours) so nothing writes to a real card; add that to the existing headless launch if it is missing.

- [ ] **Step 3: Implement `static void writeOsdStyle()` in `ui/src/settings.cpp`**

Called once where the palette is built (the same place `applyTheme`'s result becomes `p`), writing the three lines with `std::ofstream` through a temp file and `rename` (the daemon and RetroArch may read it at any moment). Values come from the same palette the `osd` box uses (`p.panelBody`, `p.edge`) and `theme::themeCrisp()`.

- [ ] **Step 4: Run the test** — same command; expected: PASS, and the file survives a theme change with the new theme's colours (add that case to the test: switch theme, assert `bg` changed).

- [ ] **Step 5: Commit** — `git commit -m "ui: the menu tells the game what its cards look like"`

---

### Task 2: The pill and the achievement card in RetroArch

**Files:**
- Create: `buildroot/patches/retroarch/0014-banana-osd.patch`
- Test: none automated (a RetroArch patch cannot run host-side); the check is Step 2's dry run plus Task 6.

**Interfaces:**
- Consumes: `/data/banana/osd-style` (Task 1).
- Produces: `SHOW_MSG BN V <0-100>` and `SHOW_MSG BN B <0-100>` draw the pill; anything else unchanged.

- [ ] **Step 1: Write the patch against a clean tree**

Unpack `v1.22.2` to `/tmp/opencode/RetroArch-1.22.2` (already there), edit, and `diff -u` the three files into `0014-banana-osd.patch` with `a/`/`b/` prefixes.

- `command.c` `command_show_osd_msg()` (~:733): if `arg` starts `BN V ` or `BN B ` and the tail parses as 0–100, push with duration `90` (1.5 s) and `MESSAGE_QUEUE_ICON_DEFAULT`; otherwise push exactly as today with `180`.
- `gfx/gfx_widgets.c` (message draw, ~:1150-1400): before the existing geometry, `if (banana_osd_parse(msg->msg, &kind, &pct))` → `banana_osd_draw_pill()` and return. The pill: card = the widget's existing rounded background draw, tinted from `/data/banana/osd-style` (`bg`; fallback `0x211f26`), at 310x42 scaled by `video_height / 557.0f`, centred horizontally, `4 * scale` from the top; icon = the 12x12 `VOL`/`MUTE`/`SUN` grids from `settings.cpp:1171-1177` drawn as quads at `k = max(2, round(2 * scale))`; 10 segments with gap `max(2, round(3 * scale))`; percent right-aligned in `0xffe68a`. `banana_osd_parse` requires the exact prefix, a space, and a tail of 1–3 digits only.
- `gfx/widgets/gfx_widget_achievement_popup.c`: the backdrop quad's tint from `osd-style` `edge` at low alpha over `bg` (fallback: today's texture), the subtitle text in `0xffe68a`, the title (the English "Achievement Unlocked – %) header) not drawn at all.

A `shortcut:` comment on the `557.0f` constant: it is the logical canvas height, and a different panel would need it revisited.

- [ ] **Step 2: Verify it applies cleanly on top of the twelve patches already carried**

Run, in a scratch tree:
`rm -rf /tmp/opencode/ra-check && mkdir /tmp/opencode/ra-check && tar -xf /tmp/opencode/RetroArch-1.22.2.tar.gz -C /tmp/opencode/ra-check --strip-components=1 && for p in /home/tom/banana-rpc/buildroot/patches/retroarch/*.patch; do patch -p1 -d /tmp/opencode/ra-check -s < "$p" || echo "FAIL $p"; done && patch -p1 --dry-run -d /tmp/opencode/ra-check < /home/tom/banana-rpc/buildroot/patches/retroarch/0014-banana-osd.patch`
Expected: no `FAIL` lines, and the dry run of `0014` reports the three files patched with no `FAILED`/`Hunk`. The earlier patches matter: `0012` already edits `command.c`, and `0011` edits the OSD/widgets path.

- [ ] **Step 3: Apply it for real in the same tree and compile-check the two touched units**

Run: `patch -p1 < …` then `gcc -fsyntax-only -I. -Ideps -DHAVE_CONFIG_H gfx/gfx_widgets.c` — if the tree has no `config.h`, skip this and say so in the commit message; the full build in Task 6 is the real gate.

- [ ] **Step 4: Commit** — `git commit -m "retroarch: the volume pill and the achievement card are Banana's"`

---

### Task 3: The launcher hands over the font and the HUD switch

**Files:**
- Modify: `system/rootfs/usr/libexec/banana/run-retroarch` (near the `set_cfg` block :212-270 and the cheevos block :353-371)
- Test: `system/tests/run-retroarch-test.sh`

**Interfaces:**
- Consumes: the `hud` key in `/data/banana/bananad.conf` (default `1`).

- [ ] **Step 1: Write the failing test** — in `run-retroarch-test.sh`, assert the generated config has `video_font_path = "/usr/share/banana/rnds/fonts/PixelifySans-Medium.ttf"`, a non-empty `video_font_size`, and that `cheevos_visibility_unlock` is `true` with no `hud` line and `false` with `hud = 0` in a staged `bananad.conf`.

- [ ] **Step 2: Run it and watch it fail** — `system/tests/run-retroarch-test.sh 2>&1 | tail -n 5` → the three assertions fail.

- [ ] **Step 3: Implement**

`set_cfg video_font_path "/usr/share/banana/rnds/fonts/PixelifySans-Medium.ttf"` and `set_cfg video_font_size 34` next to the other forced settings; then `hud="$(awk -F= '/^[[:space:]]*hud[[:space:]]*=/{gsub(/[[:space:]]/,"",$2); v=$2} END{print v}' /data/banana/bananad.conf 2>/dev/null)"` and `set_cfg cheevos_visibility_unlock "$([ "$hud" = 0 ] || [ "$hud" = off ] && echo false || echo true)"`.

- [ ] **Step 4: Run the test** — expected: **39 passed** (38 today, plus the visibility case).

- [ ] **Step 5: Commit** — `git commit -m "run-retroarch: Banana's font, and the HUD switch reaches the achievements"`

---

### Task 4: The daemon sends numbers

**Files:**
- Modify: `system/bananad/bananad.c` (`hud_fmt`, `volume`, `brightness`, `cheevos_poll`, `selftest_hud`)
- Test: `system/bananad/tests/hud-test.sh`

**Interfaces:**
- Produces: `SHOW_MSG BN V <n>` / `SHOW_MSG BN B <n>`.

- [ ] **Step 1: Write the failing test** — replace the volume/brightness `want` lines with `hud en: BN V 62` and `hud en: BN B 80`, and `hud es: BN V 62`; delete the achievement-text cases (`cheevos:`, `cheevos odd:`, `cheevos no points:`); keep `hud off:`, `hud no game:` and one translated battery case (`hud de: Akku schwach: 9 %`) so the language table stays covered.

- [ ] **Step 2: Run it and watch it fail** — `sh system/bananad/tests/hud-test.sh` → `FAIL: hud en: got "Volume 62 %"`.

- [ ] **Step 3: Implement**

`hud_strings[HUD_VOLUME]` becomes `"BN V %d"` in all three rows (identical, language-free) and `HUD_BRIGHT` becomes `"BN B %d"`; `HUD_LOW`/`HUD_EMPTY` keep their translations, so `lang`/`load_lang()` stay. `cheevos_line()` keeps only its parsing role: `cheevos_poll` no longer calls `hud_show` for achievements and instead touches `hud-live` when `hud_on()` and a 7-field line parsed. `selftest_hud` drops the removed cases.

- [ ] **Step 4: Run it** — `sh system/bananad/tests/hud-test.sh` → all pass; then `make -s test` → green.

- [ ] **Step 5: Commit** — `git commit -m "daemon: the pill takes the number, not the sentence"`

---

### Task 5: The documents

**Files:**
- Modify: `docs/ARCHITECTURE.md` §4, `docs/ROADMAP.md`

- [ ] **Step 1: Write** — ARCHITECTURE §4: the in-game line bullet becomes the pill (tokens, `osd-style`, the patch's three files, the achievement card and the `hud` switch). ROADMAP: moving the in-game line entry to *In the image, not yet released* style Done entry at **0.5.12** naming the patch.
- [ ] **Step 2: `make -s test` and `make -s agent-check`** — both green.
- [ ] **Step 3: Commit** — `git commit -m "docs: the pill is what the panel shows"`

---

### Task 6: Build the image and look at it

**Files:** none (verification only)

- [ ] **Step 1:** `make retroid-pocket-classic` — must compile RetroArch with the patch; grep the log for `0014-banana-osd` and for no `FAILED`/`Hunk`.
- [ ] **Step 2:** Write the card (or push the update), boot, open a game, and check on the panel: volume key → pill with the right segments and percent; brightness → sun pill; achievement → badge card without the English header; save state → the old-style message, unchanged; `hud = 0` → nothing, and the menu still toasts after the game; switch to a colour theme → the pill follows.
- [ ] **Step 3:** Record what you saw in the ledger (or in the final message), including anything the panel disagreed with.

## Self-Review

- Spec coverage: pill (Task 2), theme following (Task 1 + 2), font (Task 3), `hud` gating (Task 3), achievements (Task 2 + 4), tokens (Task 4), docs (Task 5), verification (Task 6). The spec's "no new process/socket" holds.
- The battery lines stay plain messages, as the spec says — no task changes them, and Task 4's `hud_fmt` keeps `HUD_LOW`/`HUD_EMPTY`.
- Types: `hud_fmt(key, value, buf, n)` keeps its signature; only the strings change.
- Review Focus pinning: exact-prefix parsing and the missing-file fallback are properties of Task 2's parser, asserted by the dry-run and the panel run (no host test exists); ordinary messages, double presses, and `hud = 0` are Task 6's panel checks because only the panel can see them.
