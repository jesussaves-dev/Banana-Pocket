# Banana OSD — the pill inside games

**Status:** design, awaiting approval
**Date:** 2026-11-05
**Approach chosen:** patch RetroArch's on-screen drawing (option 1 of three, picked by the operator)

## Intent

Inside a game, the panel belongs to RetroArch, and RetroArch's notification is a flat grey box in the
bottom-left corner. The menu already has a Banana look: a 310x42 pill at the top of the panel, a 12x12
pixel speaker/sun icon, a 10-segment bar and the percent in gold. The operator wants that same pill for
volume, brightness and achievements while a game runs, instead of RetroArch's box.

The whole point is that the OS keeps its voice in game: the same shape, the same font, the same colours as
the menu's own level card.

## What exists to build on

- `ui/src/settings.cpp` `Settings::level()` draws the pill today: 310x42 at `y0 = 4`, radius from the
  `osd` box style, `VOL`/`MUTE`/`SUN` 12x12 ASCII glyph grids (the icon is a pixel grid, not a bitmap),
  a 10-segment bar, percent right-aligned in `0xffe68a`, text `0xf1ede2`, unlit segments `0x3a3540`,
  mute icon `0xff8a96`.
- `bananad` already sends lines into the game: `SHOW_MSG <text>` written to `/run/banana/retroarch.cmd`
  (`ra_cmd()`), which is RetroArch's stdin FIFO. `SHOW_MSG` lands in `command_show_osd_msg()`
  (`command.c:733`) and becomes `runloop_msg_queue_push(arg, len, 1, 180, ...)`.
- RetroArch 1.22.2's message widget (`gfx/gfx_widgets.c`) already draws a rounded background, an icon from
  its widget atlas, an optional progress bar and the text, with slide/fade animation and a configurable
  duration. It is a box with a left icon and a full-width bar — close in furniture, wrong in style.
- RetroArch's achievement popup (`gfx/widgets/gfx_widget_achievement_popup.c`, 643 lines) already draws a
  card with the achievement badge, title and points, with its own anchor settings. It is the only place
  badge art exists in game.
- `video_font_path` and `video_font_size` are exposed (verified from the unit's binary) and feed the
  message widget's font (`gfx/gfx_widgets.c:841-847`). Banana's pixel font is already on the device at
  `/usr/share/banana/rnds/fonts/PixelifySans-{Regular,Medium}.ttf`.

## Approaches considered

1. **Patch RetroArch's message draw (chosen).** A Buildroot patch adds a "banana pill" style to the
   existing message widget and restyles the achievement popup. One carried patch, one rebuild.
2. **Config-only restyle.** `video_font_path`, `video_msg_color_*`, `video_msg_bgcolor_*`,
   `video_message_pos_x/y`. Gets Banana's font and colours and takes the box away, but there is no pill,
   no icon and no segmented bar — RetroArch exposes text colour and a flat background only.
3. **An overlay of our own above the game.** `cage` is the compositor during play and exposes no
   layer-shell; RetroArch is its only client. This means replacing the game handoff with a compositor we
   write or configure. Rejected on cost and risk.

Approach 1 is approved by the operator (AGENTS.md §4 requires that for a `buildroot/patches/retroarch/`
change; userspace config provably cannot draw the shape, per approach 2).

## Goals

- Volume and brightness while a game runs show the menu's pill, top centre: icon, 10-segment bar, percent.
- Achievements while a game runs show a Banana-styled card with the badge, the title and the points.
- The OSD text uses Banana's pixel font.
- Everything else RetroArch says (save state, screenshots, fast-forward, a core's own messages) still
  works, unchanged, in the old style — the patch must not swallow ordinary messages.

## Non-goals

- BananaTube and PortMaster: they draw their own OSD; untouched.
- RetroArch's menu chrome, quick menu, ozone theme.
- Changing what bananad decides to show (the bridge's event rules stay as shipped in 0.5.11).
- A new process, socket or overlay.
- RetroArch's own volume widget (`gfx/widgets/gfx_widget_volume.c`): the hardware keys never reach it
  (they are unbound in RetroArch), so it only appears if someone changes volume through RetroArch's own
  menu. Left as it is.

## The design

### Channels

`bananad` keeps writing to the existing FIFO and adds a token to the two messages it cares about:

- `SHOW_MSG BN V <0-100>` — volume
- `SHOW_MSG BN B <0-100>` — brightness

Everything else it sends stays plain (`PAUSE_TOGGLE`, `AUDIO_REINIT`, and any future plain message).

The achievement channel moves **back to RetroArch's own popup**, which the bridge disabled in 0.5.11 to
stop double notifications. With the popup restyled this is one card, with badge art, and it is the only
channel; bananad stops sending achievement text and keeps writing `/run/banana/hud-live` when a game is
running and `hud` is on, so the menu still knows not to repeat the unlocks afterwards.

### The patch

One file, `buildroot/patches/retroarch/00XX-banana-osd.patch`, touching:

1. `command.c` — `command_show_osd_msg()` recognises the `BN V <n>` / `BN B <n>` prefix, strips it, and
   pushes the queue entry with a Banana style marker (a flag bit) plus the number. Everything else is
   pushed exactly as today.
2. `gfx/gfx_widgets.c` — the message draw path renders the marked entry as the pill: card, icon, ten
   segments, percent; the unmarked path is untouched code. Geometry is the menu's, scaled from the
   640x557 logical canvas to the real video size (`video_height / 557.0f`), so the pill is the same
   physical size on the panel. Icons are the same 12x12 grids from `settings.cpp`, drawn as quads, so no
   asset file is added. Colours are the menu's constants.
3. `gfx/widgets/gfx_widget_achievement_popup.c` — colours, font and corner radius to Banana's palette; its
   anchor stays configurable through RetroArch's own setting.

The pill appears where the menu's does (top centre), lasts ~1.5 s, and is replaced rather than queued when
a second volume press arrives.

### Following the theme

The menu owns theme resolution, so it hands the answer over: when `banana-ui` applies a theme (at startup and
whenever the theme changes in Settings) it writes `/run/banana/osd-style` — the card fill, the edge colour and
the corner radius it is drawing with, plus whether the theme is `crisp` (radius 0). The patch reads that file
once at widget init and falls back to the default dark card if it is missing or unreadable, so an older menu
still gets a sane pill. No environment plumbing and no second parser: the daemon and the launcher stay out of it.

### Configuration

`run-retroarch` gains `video_font_path` (Pixelify Medium) and a `video_font_size` that matches the panel.
It also sets `cheevos_visibility_unlock` from the `hud` key in `/data/banana/bananad.conf` (default on), so the
Settings row's promise — "the screen is the game's alone" — holds for the achievement card too. If the font
fails to load, RetroArch falls back to its built-in font and everything else still works.

The achievement card shows the badge, the achievement's title and its points. RetroArch's own header
("Achievement Unlocked – 5%") is hidden by the patch: it is English-only while the OS speaks three languages,
and the menu's toast never showed it either.

### What the menu and daemon lose

`bananad`'s `cheevos_line()` formatting of achievement text (`"<title> (+<pts>)"`) and its `--selftest hud`
cases for it are deleted: the popup owns that text now. `hud_fmt()`'s `HUD_VOLUME`/`HUD_BRIGHT` strings
become the token (`BN V %d`), so the daemon no longer carries translated volume/brightness text. The
`HUD_LOW`/`HUD_EMPTY` battery lines stay plain messages, in the old style — a warning is not a level.

`hud-live` stays, written by the daemon's cheevos poll when `hud_on()`, so the menu keeps suppressing its
post-game replay.

## Files

- New: `buildroot/patches/retroarch/00XX-banana-osd.patch`
- `system/rootfs/usr/libexec/banana/run-retroarch` — font, size, `cheevos_visibility_unlock` from `hud`
- `ui/src/settings.cpp` (or wherever the theme is applied) and `ui/src/theme.cpp` — write `/run/banana/osd-style`
- `system/bananad/bananad.c` — the two tokens, drop achievement text formatting
- `system/bananad/tests/hud-test.sh` — token assertions, the removed cases
- `system/tests/run-retroarch-test.sh` — the font, visibility and `hud = 0` lines
- `docs/ARCHITECTURE.md` §4, `docs/ROADMAP.md` (Done entry, 0.5.12)

## Verification

- The patch applies to a clean 1.22.2 tree (`patch -p1 --dry-run`) before the build.
- `make -s test` and `make -s agent-check`.
- One full `make retroid-pocket-classic`, then one game on the panel: volume key shows the pill with the
  right segment count and percent; brightness likewise; an achievement shows the badge card; a save state
  still shows the old-style message (the patch did not swallow it); `hud = 0` shows nothing, achievement
  card included; switching to a colour theme and repeating the volume key shows the pill in that theme's
  card and edge colours.
- `make sound-test BANANA_HOST=…` is not needed: no audio path changes.

## Risks

- The patch is carried across RetroArch upgrades; it touches three files in `command.c`, `gfx/gfx_widgets.c`
  and one widget, which is small enough to rebase but will need the rebase at each bump.
- RetroArch's font loading depends on the image's assets; if `video_font_path` fails the pill still draws
  with the default font (the look degrades, the function does not).
- The pill's scale constant is tied to the 640x557 logical canvas; a future resolution change needs it
  revisited (`shortcut:` comment in the patch).

## Left to the builder

- Corner radius, icon size and the exact pill width when the text is long.
- Whether the bar animates between steps or jumps.
- Slide-in animation: reuse RetroArch's (`gfx_animation`) or appear at once.
- The token spelling (`BN V 62` is my call; anything short and unambiguous works).
