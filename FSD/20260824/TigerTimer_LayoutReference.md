# Tiger Timer — Layout Reference & Assets

**Purpose of this document:** the FSD (`TigerTimer_FSD.md`) describes *what* each screen does.
This document describes the **visual system** that makes ~20+ screens feel consistent — the
small set of reusable conventions (margins, icon sizing, header treatment, divider placement)
that every screen is built from. It is deliberately **not** a coordinate-by-coordinate
transcription of every screen — that information already exists, exactly and authoritatively, in
the source code (every draw call states its own literal pixel numbers). Duplicating it here would
be redundant and would drift out of sync the moment either changed.

Instead, this document gives you:
1. The shared layout system (below), with real numbers pulled directly from the source.
2. Pixel-accurate **rendered reference images** for one representative screen of each distinct
   layout pattern (not all ~20 screens individually — most of them are variations on the same
   4-6 patterns; see the mapping table below for which pattern each screen uses).
3. An inventory of the actual visual **assets** (bitmap logo, custom font) with real extracted
   files, not just descriptions.

All coordinates below were read directly from the current source (`tiger_timer.ino` /
`web_config.ino`), not from memory — if you change the code, this document (and the renders)
will need a manual refresh to stay accurate; it is a snapshot, not a live-generated artifact.

---

## 1. The Screen Coordinate System

- Physical panel: 128×64 pixels, monochrome (1 bit/pixel), origin `(0,0)` at top-left, `x`
  increases rightward, `y` increases downward — standard raster convention.
- Two games (Tetris, Star Drop) temporarily call `display.setRotation(1)` for their own draw
  calls only, which swaps the *logical* coordinate space to 64 wide × 128 tall for the duration
  of that one screen, then calls `setRotation(0)` before returning. Every other screen always
  works in the normal 128×64 space. See pattern F below.
- Text is measured and positioned via `display.getTextBounds()` before drawing, not hardcoded
  guessed widths — this is how translated strings of very different lengths (English vs. French
  vs. Spanish) all still end up correctly centered or right-aligned. **Any reimplementation
  should do the same** — hardcoding pixel widths per string will silently break the moment the
  active language changes.
- Default font: Adafruit GFX's built-in 5×7 font, ~6px horizontal advance per character at
  `textSize(1)` (5px glyph + 1px spacing), 8px line height. Scales linearly with `setTextSize(n)`.
- A second, custom 8×8 pixel font (Press Start 2P) is used in exactly four places: the boot
  splash wordmark, the per-game splash screen's game-name text, and the Tetris/Star Drop title
  bars. See §3.

---

## 2. The Six Layout Patterns

Every screen in the app is a variation on one of these six patterns.

### Pattern A — "Standard template" (the most common pattern by far)
Used by: Timer, Reflex, Dice (both screens), Chess (all three screens), Pass the Bomb, Turn
Order, Stopwatch, Toothbrush (both screens), Trivia's category/level pickers, Settings, Language
picker, Timer Step picker.

| Element | Position | Notes |
|---|---|---|
| Game icon | center `(7,7)`, radius `6` | Always top-left, always this exact size |
| Header/status text | right-aligned: `x = 128 - textWidth - 2`, `y = 4` | Not present on every screen (a few, like Settings/Language, have no header text and just leave that space empty) |
| Divider line | `(0,14)` to `(128,14)`, full width | Drawn even when there's no header text above it |
| Content area | `y = 15` to `63` | Whatever that screen needs — big centered numbers, lists, etc. |

**Reference render:** `renders/pattern_A_standard_template.png` (using the Timer game as the
example — icon, right-aligned "GO!" status, divider, big centered "2:00").

### Pattern B — Main menu list
One screen only (`drawMenuScreen`).

| Element | Position |
|---|---|
| Volume icon | top-left, `(2,1)` |
| Screen title ("Choose Option", translated) | centered, `y = 2` |
| List starts | `y = 14` (`LIST_TOP`) |
| Row height | `16px` each, `3` rows visible at once |
| Selected row highlight | `fillRect(10, rowTop, 108, 14)` — inverted (filled, black text) |
| Row icon | `cx = 24`, vertically centered in its row, radius `6` |
| Row label | `x = 40`, vertically centered in its row |

**Reference render:** `renders/pattern_B_menu_list.png`.

### Pattern C — Centered dialog (Yes/No)
One screen only (the leave-confirmation dialog).

| Element | Position |
|---|---|
| Title text | centered, `y = 20` |
| Two boxes | `40×20px` each, `10px` gap between, horizontally centered as a pair, `y = 38` |
| Selected box | filled (inverted); unselected | outlined only |

**Reference render:** `renders/pattern_C_leave_confirm.png`.

### Pattern D — Two-column split
One screen only (Pass the Bomb's challenge display), built on top of Pattern A's header/divider.

| Element | Position |
|---|---|
| Left column | `x = 0` to `85` (`= 128 × 2/3`), category text, word-wrapped, starting `y = 24` |
| Column divider | vertical line `x = 85`, `y = 16` to `62` |
| Right column | `x = 85` to `128`, "Letter" label at `y = 22`, the letter itself at `textSize(3)`, `y = 34` |

**Reference render:** `renders/pattern_D_pass_the_bomb.png`.

### Pattern E — QR + text (Config Mode)
One screen only.

| Element | Position |
|---|---|
| QR code | top-left `(2, (64-48)/2)`, `48×48px` |
| Text column | starts at `x = QR_right_edge + 4` |
| Six text lines | `y = 2, 11, 20, 32, 41, 50` — "WiFi:" / SSID line 1 / SSID line 2 / "Connect or" / "visit" / IP address |

**Reference render:** `renders/pattern_E_config_mode.png`.

### Pattern F — Rotated full-screen game board (Tetris, Star Drop)
Both games call `setRotation(1)` before drawing, giving a **64 wide × 128 tall** logical canvas
for these draw calls only.

| Element | Position (in the rotated 64×128 space) |
|---|---|
| HUD strip | `y = 0` to `8`, text at `(2, 0)` |
| HUD divider | horizontal line at `y = 7` (Tetris: `TETRIS_HUD_HEIGHT - 1`) |
| Board | starts at `y = 8`, cells `8×8px` each (7×7 drawn, 1px gap) |
| Tetris board | 8 columns × 15 rows (exactly fills the remaining 64×120) |
| Star Drop board | 4 lanes × 16px each (exactly fills the 64px width) |

**Reference render:** `renders/pattern_F_tetris_rotated.png` — note this image is oriented in
the *rotated* 64×128 frame, i.e. this is what the panel's pixels actually contain; physically
rotating the device 90° is what makes it read correctly to the player.

### (Screens not covered by A–F)
A handful of screens have genuinely one-off layouts not worth generalizing into a "pattern"
because nothing else reuses them: the boot splash (§3), the per-game splash screen, Trivia's
inverted-row-with-arrows play screen, and Tetris/Star Drop's inverted-title-bar Ready/Game-Over
screens. These are each fully described in prose in the FSD (§4.1, §4.3, §5.8, §5.10/§5.11
respectively) and are small enough that the source code is the fastest way to get their exact
numbers — each is one self-contained `draw...Screen()` function, typically under 40 lines.

---

## 3. Visual Assets

Unlike the icons (which are pure vector code — `fillCircle`, `fillTriangle`, etc. — and have no
separate asset file; the source code *is* the asset), two things in this project are genuine
embedded binary assets, included here as actual files, not just descriptions:

| Asset | File | Source location |
|---|---|---|
| Boot splash tiger logo, 80×53px | `assets/tiger_logo_80x53.png` | `TIGER_BITMAP[]` in `tiger_timer.ino`, decoded byte-for-byte |
| "TIGER" wordmark, 40×8px | `assets/word_TIGER_40x8.png` | `WORD_TIGER_BITMAP[]` |
| "TIMER" wordmark, 40×8px | `assets/word_TIMER_40x8.png` | `WORD_TIMER_BITMAP[]` |
| Custom pixel font (Press Start 2P, 8×8/char) | `press_start_2p_font.h` (in the source zip) | Converted from a user-provided TTF; already an Adafruit_GFX-format font, ready to `setFont()` with directly |

The three PNGs were produced by decoding the actual `PROGMEM` byte arrays from the source
(1 bit per pixel, MSB-first, row-major, rows padded to whole bytes — standard `drawBitmap()`
format) — they are pixel-exact reproductions of what the display shows, not redrawn
approximations. If the source bitmap data ever changes, these PNGs would need regenerating from
it to stay accurate (a short script, not a manual redraw).

The font file is already in Adafruit_GFX's native format and can be used as-is by any
Arduino/ESP32 project using that display library — no conversion needed.

---

## 4. How to Use This With a Smaller Model

Recommended context to hand over per task, from smallest to largest:
1. The relevant **FSD section** (behavior).
2. If the task touches a screen using patterns A–F, **this document's matching pattern section**
   (layout) plus its reference render (visual ground truth).
3. If the task touches the splash screens or custom font, **§3's asset files** directly.
4. Only if actually stuck or verifying, the **corresponding `draw...Screen()` function** in the
   source as the ultimate ground truth for exact pixel numbers.

This keeps each task's context small (matching the dev plan's per-task FSD-section pointers)
while still giving access to real visual/binary ground truth, not just prose descriptions of it.
