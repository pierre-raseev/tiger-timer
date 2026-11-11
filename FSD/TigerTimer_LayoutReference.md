# Tiger Timer — Layout Reference & Assets

**Purpose of this document:** the FSD (`TigerTimer_FSD.md`) describes *what* each screen does.
This document describes the **visual system** that makes ~25+ screens feel consistent — the
small set of reusable conventions (margins, icon sizing, header treatment, divider placement)
that every screen is built from. It is deliberately **not** a coordinate-by-coordinate
transcription of every screen — that information already exists, exactly and authoritatively, in
the source code (every draw call states its own literal pixel numbers). Duplicating it here would
be redundant and would drift out of sync the moment either changed.

Instead, this document gives you:
1. The shared layout system (below), with real numbers pulled directly from the source.
2. Pixel-accurate **rendered reference images** for one representative screen of each distinct
   layout pattern (not all ~25 screens individually — most of them are variations on the same
   handful of patterns; see the mapping table below for which pattern each screen uses).
3. An inventory of the actual visual **assets** (bitmap logo, custom font) with real extracted
   files, not just descriptions.

**This is a snapshot, not a live-generated artifact.** All coordinates below were re-verified
directly against the current source at the time of this revision (which now includes Snake,
SimpleMorse, the reworked Pass the Bomb, Tetris's ghost piece, and the Remote Config rename) — if
you change the code further, this document (and the renders) will need a manual refresh to stay
accurate.

---

## 1. The Screen Coordinate System

- Physical panel: 128×64 pixels, monochrome (1 bit/pixel), origin `(0,0)` at top-left, `x`
  increases rightward, `y` increases downward — standard raster convention.
- Two games (Tetris, Star Drop) temporarily call `display.setRotation(1)` for their own draw
  calls only, which swaps the *logical* coordinate space to 64 wide × 128 tall for the duration
  of that one screen, then calls `setRotation(0)` before returning. Snake and SimpleMorse were
  both deliberately built to stay in the normal 128×64 orientation instead — Snake because a wide
  grid suits the panel's natural shape, SimpleMorse because it has no board to speak of. Every
  other screen always works in the normal 128×64 space.
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

## 2. The Layout Patterns

Every screen in the app is a variation on one of these patterns. Six are shared by most of the
app's ~25 screens; Snake and SimpleMorse each introduce one additional pattern of their own,
described at the end of this section.

### Pattern A — "Standard template" (the most common pattern by far)
Used by: Timer, Reflex, Dice (both screens), Chess (all three screens), Turn Order, Stopwatch,
Trivia's category/level pickers, Toothbrush Timer (both screens), Settings, Language picker,
Timer Step picker, Snake (all 4 states), SimpleMorse (all 5 states).

| Element | Position | Notes |
|---|---|---|
| Game icon | center `(7,7)`, radius `6` | Always top-left, always this exact size |
| Header/status text | right-aligned: `x = 128 - textWidth - 2`, `y = 4` | Every game now shows *something* here, even ones with no dynamic status — e.g. Stopwatch and Turn Order show that game's own translated name; this used to be a real gap in a few games and was deliberately fixed |
| Divider line | `(0,14)` to `(128,14)`, full width | Drawn even when there's no header text above it |
| Content area | `y = 15` to `63` | Whatever that screen needs — big centered numbers, lists, grids, letter/code boxes, etc. |

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

Settings (now 3 rows: Language, Timer Step, Remote Config) uses this exact same list pattern at
a smaller scale (3 fixed rows, no scrolling needed since only 3 items exist).

**Reference render:** `renders/pattern_B_menu_list.png`.

### Pattern C — Centered dialog (Yes/No)
One screen only (the leave-confirmation dialog).

| Element | Position |
|---|---|
| Title text | centered, `y = 20` |
| Two boxes | `40×20px` each, `10px` gap between, horizontally centered as a pair, `y = 38` |
| Selected box | filled (inverted); unselected | outlined only |

**Reference render:** `renders/pattern_C_leave_confirm.png`.

### Pattern D — Two-column split with a bottom hint row (**reworked this revision**)
One screen only (Pass the Bomb's challenge display), built on top of Pattern A's header/divider.
This pattern changed meaningfully since the last revision of this document — the category text
is now vertically centered, and the reroll hint row moved from left-icon/right-text to
right-icon/left-text.

| Element | Position |
|---|---|
| Divider (vertical, column split) | `x = 85` (`= 128 × 2/3`), `y = 16` to `48` (shortened from the full height to leave room for the bottom row below) |
| Left column (category text) | `x = 0` to `85`; vertically centered within `y = 16..48` accounting for whether it wraps to 1 or 2 lines, `+3px` fine-tune offset |
| Right column ("Letter" label) | `x = 85` to `128`, label at `y = 18` |
| Right column (the letter itself) | `textSize(3)`, `y = 27` |
| Bottom hint row | `14px` band, `y = 50` to `64`, **inverted** (drawn normally then the whole strip's pixels flipped) — shown only in the "ready to arm" state |
| Bottom row text ("Rotate: shuffle") | left-aligned, `x = 2` |
| Bottom row rotate-arrow icon | `cx = 128 - 9 = 119`, radius `5` — deliberately on the **right**, physically closest to where the encoder actually sits on the device |

**Reference render:** `renders/pattern_D_pass_the_bomb.png` (regenerated this revision to match
the current layout — if you have an older copy of this render, discard it, the icon/text sides
were swapped and the category text is now vertically centered rather than fixed near the top).

### Pattern E — QR + text (Remote Config, renamed from "Config Mode")
One screen only. The QR is drawn at a whole 2px per module (version 3, 29 modules = 58px): an
earlier 48px fractional size mixed 1px and 2px modules and was hard for phones to scan.

| Element | Position |
|---|---|
| QR code | top-left `(1, 3)`, `58×58px` (29 modules × 2px) |
| Text column | starts at `x = 63` (QR right edge + 4) — 11 characters max per line |
| Six text lines | `y = 2, 11, 20, 32, 41, 50` — "WiFi:" / SSID line 1 / SSID line 2 / "Connect or" / "visit" / IP address (French splits as "Connectez" / "ou visitez" to fit 11 chars) |

**Reference render:** `renders/pattern_E_remote_config.png` (predates the 58px QR — shows the old
48px layout).

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
| **Tetris ghost piece (new this revision)** | same cell grid as the real piece, drawn as `drawRect` **outlines** at the row the piece would land on if hard-dropped from its current column/rotation, drawn *before* the real (filled) piece each frame |

**Reference render:** `renders/pattern_F_tetris_ghost.png` (regenerated this revision to show
the ghost piece — an outlined O-piece near the bottom, with the real filled piece still falling
higher up).

### Pattern G — Grid game, normal orientation (Snake) — *new this revision*
One screen (well, 4 states, but one shared drawing structure), built on top of Pattern A.

| Element | Position |
|---|---|
| Content top | `y = 16` (`SNAKE_HUD_HEIGHT`) — starts just below Pattern A's divider, no separate HUD strip needed since the header text already shows the score |
| Grid | 32 columns × 12 rows, `4×4px` cells (`3×3` drawn, 1px gap), exactly filling `128×48px` |
| Snake body | filled cells |
| Food | a small filled circle within its cell |

Ready/Game Over/Paused states all reuse Pattern A's header+divider with different status text
("Ready" / "Score: N" / "Paused" / "Game Over") and, on Ready/Game Over, a centered high-score
line below the main prompt.

### Pattern H — Letter/code boxes (SimpleMorse) — *new this revision*
Two closely related sub-patterns, both built on top of Pattern A, sharing one column-per-letter
layout idea:

**H1 — Word + code (Ready screen, and the post-result solution reveal):**

| Element | Position |
|---|---|
| Column width | `128 ÷ letter count` (scales automatically to word length) |
| Letter | centered in its column, `y = 30` (Ready) or `y = 36` (solution reveal) |
| That letter's Morse code | centered in the same column, `10px` below the letter |

**H2 — Per-letter progress boxes (shown only during input):**

| Element | Position |
|---|---|
| Column width | same `128 ÷ letter count` formula as H1 |
| Box size | `min(columnWidth - 4, 20)` px square, centered in its column, `y = 32` |
| Box state | outline + normal-color letter inside (not yet completed this round) → filled solid + inverted-color letter inside (that letter's full code correctly repeated so far this round) |

Never shows Morse code notation during H2 — only which letters are done, matching the FSD's
explicit "don't give away the solution during play" requirement.

**No new reference renders for Patterns G/H this revision** — both are simple enough (grid of
squares; column of letter+code pairs) that a rendered mockup adds little beyond what the tables
above already convey, and Snake/SimpleMorse's actual draw functions are each under ~60 lines in
the source, making them fast to read directly if you need the literal numbers.

### (Screens not covered by A–H)
A handful of screens have genuinely one-off layouts not worth generalizing into a "pattern"
because nothing else reuses them: the boot splash (§3), the per-game splash screen, Trivia's
inverted-row-with-arrows play screen, and Tetris/Star Drop's inverted-title-bar Ready/Game-Over
screens. These are each fully described in prose in the FSD (§4.1, §4.3, §5.8, §5.10/§5.11
respectively) and are small enough that the source code is the fastest way to get their exact
numbers — each is one self-contained `draw...Screen()` function, typically under 80 lines.

---

## 3. Visual Assets

Unlike the icons (which are pure vector code — `fillCircle`, `fillTriangle`, `drawLine` arcs,
etc. — and have no separate asset file; the source code *is* the asset), two things in this
project are genuine embedded binary assets, included here as actual files, not just
descriptions:

| Asset | File | Source location |
|---|---|---|
| Boot splash tiger logo, 80×53px | `assets/tiger_logo_80x53.png` | `TIGER_BITMAP[]` in `tiger_timer.ino`, decoded byte-for-byte |
| "TIGER" wordmark, 40×8px | `assets/word_TIGER_40x8.png` | `WORD_TIGER_BITMAP[]` |
| "TIMER" wordmark, 40×8px | `assets/word_TIMER_40x8.png` | `WORD_TIMER_BITMAP[]` |
| Custom pixel font (Press Start 2P, 8×8/char) | `press_start_2p_font.h` (in the source zip) | Converted from a user-provided TTF; already an Adafruit_GFX-format font, ready to `setFont()` with directly |

These are unchanged since the last revision of this document — nothing about the boot splash or
font changed this session. The three PNGs were produced by decoding the actual `PROGMEM` byte
arrays from the source (1 bit per pixel, MSB-first, row-major, rows padded to whole bytes —
standard `drawBitmap()` format) — they are pixel-exact reproductions of what the display shows,
not redrawn approximations.

The icon-heavy additions this session (Snake's zigzag+head icon, SimpleMorse's walrus icon and
its wounded/normal expression variants, the reworked WiFi icon, the reworked volume
speaker-and-arcs design, Tetris's ghost-piece outline) are all pure vector code, like every other
icon in the app — no new binary assets were added.

---

## 4. How to Use This With a Smaller Model

Recommended context to hand over per task, from smallest to largest:
1. The relevant **FSD section** (behavior).
2. If the task touches a screen using patterns A–H, **this document's matching pattern section**
   (layout) plus its reference render if one exists (visual ground truth).
3. If the task touches the splash screens or custom font, **§3's asset files** directly.
4. Only if actually stuck or verifying, the **corresponding `draw...Screen()` function** in the
   source as the ultimate ground truth for exact pixel numbers.

This keeps each task's context small (matching the dev plan's per-task FSD-section pointers)
while still giving access to real visual/binary ground truth, not just prose descriptions of it.
