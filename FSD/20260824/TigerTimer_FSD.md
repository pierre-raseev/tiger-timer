# Tiger Timer — Functional Specification Document

**Document purpose:** This FSD is written to be self-contained. It describes *what* the device
does and *why*, in enough detail that a competent firmware engineer (or an AI coding model)
could implement it from this document alone, without needing the existing source code as a
starting point. Where exact pixel-level visual fidelity isn't practical to fully specify in
prose, that's called out explicitly (see "Known Limits of This Document" at the end) — for
pixel-perfect reproduction, the actual source files are the ground truth and should be used
alongside this FSD, not instead of it.

Product name: **Tiger Timer**. On-device boot splash reads "TIGER" then "TIMER" as two words
merging together (see §4.1).

---

## 1. Overview

Tiger Timer is a battery-powered, pocket-sized party-game console built on an ESP32
microcontroller with a small monochrome OLED screen, one rotary encoder, one large arcade-style
push button, and a piezo buzzer. It boots into a scrollable menu of 11 mini-games plus a
Settings and an Instructions entry. Each game is self-contained, uses the same two physical
controls (encoder + button) with a consistent global interaction model, and returns to the menu
via a confirmation dialog so gameplay is never lost by accident.

A secondary feature, **Config Mode**, turns the device into its own WiFi access point serving a
web page (from any phone or laptop) for editing settings that aren't practical to edit with a
single knob and one button: reordering/enabling games, editing trivia questions in bulk,
enabling/disabling "Pass the Bomb" categories, and choosing startup behavior.

The interface is fully localized into English, French, and Spanish, chosen via a Settings
screen and persisted across power cycles.

---

## 2. Hardware Specification

### 2.1 Microcontroller
- **Board:** Seeed Studio XIAO ESP32C6 (or pin-compatible equivalent). Uses the Arduino core's
  `D0`–`D10` pin aliases throughout this spec and the firmware.
- **Framework:** Arduino core for ESP32 (not raw ESP-IDF).

### 2.2 Display
- **Panel:** SH1106-driver monochrome OLED, 128×64 pixels, I2C interface, address `0x3C`.
- **Wiring:** SDA → `D4`, SCL → `D5`, VCC → 3.3V, GND → GND.
- **Library:** Adafruit SH110X (pulls in Adafruit GFX Library + Adafruit BusIO via Library
  Manager).
- Orientation: normally landscape (128 wide × 64 tall). Two specific games (Tetris and Star
  Drop — see §5.10, §5.11) temporarily rotate the display 90° in software only while their own
  screens are drawn, then rotate back to normal before returning control — no other screen is
  ever affected by this.

### 2.3 Input: Arcade Button
- A large illuminated arcade-style push button.
- Switch: `D1` → GND, wired `INPUT_PULLUP` (electrically LOW = pressed).
- Built-in LED: 5V, driven directly from a GPIO with no boost circuitry needed → `D3` (+GND).
  The LED is used as a persistent state indicator across nearly every game (see per-game specs).
- Debounced in software (~a few ms) before any action fires.

### 2.4 Input: Rotary Encoder
- A standard EC11-style incremental rotary encoder with integrated push-button.
- CLK → `D10`, DT → `D8`, SW (push button) → `D9`, wired `INPUT_PULLUP` (LOW = pressed).
- Rotation direction: reading `DT` on a `CLK` falling edge; `DT == HIGH` = clockwise
  (`direction = +1`), `DT == LOW` = counter-clockwise (`direction = -1`).
- The encoder's own push button is debounced independently from the arcade button and supports
  three distinct gestures: **short click**, **long-press (1 second)**, and **hold+turn**. See
  §3.2.

### 2.5 Audio
- A passive piezo buzzer (not an active/self-oscillating buzzer — it must be frequency-driven).
- Wired to `D2` (+ GND).
- Driven via the ESP32's LEDC PWM peripheral: `ledcAttach(pin, 2000, bits)` once at boot, then
  `ledcWriteTone(pin, frequency)` to play a note and `ledcWrite(pin, 0)` to silence it.
- Volume is implemented as **PWM duty cycle**, not amplitude in software mixing — three levels:
  - Mute: duty 0 (buzzer calls are skipped entirely, not silently played at 0 duty, to avoid
    wasting the delay time a note would otherwise block for).
  - Low: duty 40/255.
  - High: duty 128/255 (chosen to roughly match the loudness of the classic Arduino `tone()`
    function, for continuity with an earlier prototype).

### 2.6 Power
- **Battery:** 3.7V LiPo cell.
- **Charging:** a separate charge-management module (e.g. TP4056-style board) between the
  battery and the ESP32 — charging is electrically independent of the microcontroller, so it
  continues to work correctly even while the ESP32 is in deep sleep.
- **Physical power switch:** yes — a hard power switch exists in addition to software deep
  sleep. (Deep sleep is the *software-controlled* low-power state used automatically after
  inactivity; the physical switch is a full hardware disconnect for storage/shipping.)
- **Deep sleep wake source:** `esp_sleep_enable_ext1_wakeup` on the arcade button pin
  (`ESP_EXT1_WAKEUP_ANY_LOW`), with the RTC peripheral power domain kept on so the button's
  internal pull-up stays reliable through sleep. Waking from deep sleep is a full reboot (runs
  `setup()` again), not a resume-from-RAM.

---

## 3. Global System Behavior

### 3.1 Boot Sequence
1. Power on (or wake from deep sleep) → `setup()` runs.
2. Load all persisted settings from flash (see §3.5 for the full key list).
3. Mount the filesystem used for trivia question data (LittleFS); format it automatically on
   first-ever boot if unformatted.
4. Initialize the display, encoder pins, button pin, buzzer PWM channel.
5. Play the boot splash animation (§4.1) — **unless the arcade button is held down**, in which
   case the splash is skipped entirely and the device proceeds straight to step 6. (See §3.6 for
   why this is button-debounce-safe.)
6. Apply the configured startup behavior: either open directly on the main menu, or boot
   straight into a specific chosen game (both configurable via Config Mode, §7).

### 3.2 Global Input Model

This is the single most important section for consistent behavior across every screen. There
are exactly two physical controls, and their meaning is the same everywhere except where a game
explicitly overrides it:

| Control | Gesture | Meaning |
|---|---|---|
| Arcade button | Short press | **Confirm / primary action.** In a menu-type screen (main menu, Settings, Language picker, Timer Step picker, Instructions, the leave-confirmation dialog) this confirms whichever item is highlighted. Inside a game, this is that game's main action (start countdown, roll dice, arm the bomb, etc. — see each game's spec). |
| Encoder | Turn (not held) | Moves a selection cursor, adjusts a numeric value, or performs a game-specific action, depending on the current screen (see each game's spec). |
| Encoder | Short click (press+release, not held past ~1s, no turn while held) | **Also confirm / primary action** — functionally identical to the arcade button's short press in every menu-type screen. Inside some games it does a secondary context action instead (see the per-game "Encoder click" rows below) if that game defines one; otherwise it does nothing. |
| Encoder | Held down and turned | **Adjusts buzzer volume**, from *anywhere*, at *any time*, even mid-game. While held+turning, the screen switches to a full-screen volume indicator (§6) live-updating with every detent; releasing restores whatever screen was showing underneath. This gesture "claims" the hold, so releasing afterward does **not** also fire a click action. |
| Encoder | Held for 1 full second *without* being turned | **Opens the "Go back to menu?" dialog** if currently inside a game (§3.4). If already anywhere in the menu/settings/instructions hierarchy, this does nothing (you're already there). If the confirmation dialog is already open, a second long-press does nothing extra. This is a single consistent rule everywhere — there is no per-game exception to it. |

Two important interaction guarantees:
- A press-and-hold that gets claimed by rotation (volume) **never** also triggers a click action
  on release, and never fires the long-press-menu gesture either (they're mutually exclusive —
  whichever happens first, wins).
- If a hold reaches the 1-second long-press threshold *before* any rotation occurs, the menu
  dialog opens immediately (continuously checked, not just evaluated on release) — and if the
  user *keeps holding and starts turning after that*, the turn is now interpreted as navigating
  the dialog (Yes/No), not as a volume adjustment, since the long-press action already fired.

### 3.3 Screen-Sleep and Deep-Sleep Power Management
- **Screen sleep** (OLED powered off, contents preserved in RAM): after 5 minutes of no button
  or encoder activity, *unless* something "time-critical" is actively running (a countdown, an
  armed reflex round, a running chess clock with an active player, an armed bomb, a running
  stopwatch, an active trivia buzz-in window, a running toothbrush timer, or an in-progress
  Tetris/Star Drop game) — those are never allowed to go dark mid-round. Any button/encoder
  activity wakes the screen immediately.
- **Deep sleep** (full chip shutdown, wakes via reboot): after 30 minutes of no activity, subject
  to the same "not time-critical" exemption. USB charging is unaffected by deep sleep, since the
  charge circuit is separate hardware (§2.6).

### 3.4 Leaving a Game: The Confirmation Dialog
Triggered only by the encoder's 1-second long-press (§3.2) while inside any game. Shows:
- Centered title text: "Go back to menu?" (translated).
- Two boxes side by side, "Yes" and "No", **Yes is the default highlighted choice**.
- Turning the encoder toggles which of the two is highlighted (only two options, so either
  rotation direction just flips it).
- Confirming (arcade button *or* encoder short click, per §3.2) on "Yes" performs a full,
  safe teardown of whatever was running (pauses the timer, resets an armed reflex round to
  ready, pauses the chess clock, defuses an armed bomb, pauses the stopwatch preserving elapsed
  time, cancels a trivia answer-reveal window, turns off the button LED) and returns to the main
  menu with the cursor positioned on the game just left.
- Confirming on "No" simply closes the dialog and resumes the game exactly as it was — nothing
  changes.

### 3.5 Persistence
All persisted settings live in a single non-volatile-storage namespace (ESP32 NVS via the
Arduino `Preferences` library). Keys and what they store:

| Key | Type | Meaning | Default |
|---|---|---|---|
| `lang` | uint8 | Current language index (0=EN, 1=FR, 2=ES) | 0 (English) |
| `step` | uint8 | Timer Step setting index (§5.1) | index of "Auto" |
| `deftime` | uint16 | Default Timer game length, in seconds | 120 (2:00) |
| `startMode` | uint8 | 0 = boot to menu, 1 = boot straight into a specific game | 0 |
| `startGame` | uint8 | Which game id to boot into when `startMode`=1 | Timer's id |
| `menuOrd` | bytes (N) | The current menu ordering, as a list of game/sentinel ids | the default order below |
| `gameEn` | bytes (N) | Per-game enabled/disabled flags, parallel array to the game id list | all enabled |
| `hpCatEn` | bytes (20) | Per-category enabled/disabled flags for Pass the Bomb (§5.5) | all enabled |
| `sdHigh` | uint16 | Star Drop's persisted high score | 0 |

Design rule enforced everywhere a set of things can be toggled off (games in the menu, Pass the
Bomb categories): **at least one must always remain enabled.** If a save would disable the last
remaining item, that specific part of the change is silently rejected (a warning is surfaced in
Config Mode's web UI) while any other part of the same save still applies.

### 3.6 Why the Splash-Skip Doesn't Cause a Phantom Click
Worth documenting explicitly because it's a real bug class: if you skip the boot splash by
holding the arcade button, the button is still physically down (or its release hasn't yet
settled) the instant the main loop's debounce logic starts running. A naive implementation would
misread that as a brand-new press-then-release and fire a phantom confirm action. The correct
fix: when the splash-skip is detected, **block and wait for the physical release** (plus a short
settle delay, ~30ms) before returning control to the rest of boot — so by the time the main loop
starts, the button reads cleanly "not pressed" and no phantom edge exists.

---

## 4. Menu & Navigation

### 4.1 Boot Splash Animation
A two-phase animated wordmark, skippable by holding the arcade button at any point during it
(see §3.6):
- Phase 1: a "TIGER" bitmap wordmark slides in from off-screen left to a center-left resting
  position.
- Phase 2: the "TIGER" word stays fixed; a "TIMER" bitmap wordmark slides in from off-screen
  right until the two words sit adjacent, reading "TIGERTIMER" as one merged logo.
- A short audio flourish plays as the animation completes, then it holds briefly before handing
  off to the normal startup logic (§3.1 step 6).
- Both wordmarks are pre-rendered bitmap images (not live text) using a chunky pixel/arcade-style
  font, each roughly 40×8 px per word at their native display scale — see §9 for the exact font
  used elsewhere for reproducing this style live.

### 4.2 Main Menu
- A vertically scrollable list, 3 rows visible at a time, each row showing a small icon (left)
  and the item's translated label (right of the icon). The highlighted row is shown inverted
  (filled background, dark text).
- **Default order** (used on first boot, or if saved order data is ever invalid): Timer,
  Toothbrush Timer, Dice, Reflex, Chess Clock, Pass the Bomb, Turn Order, Stopwatch, Trivia,
  Tetris, Star Drop, **Instructions**, **Settings** — Instructions and Settings are always the
  last two entries by default, and (like any entry) can be reordered via Config Mode but can
  never be disabled/hidden (§3.5).
- On boot into the menu, the cursor highlights the **first entry in the list**, not any
  previously-remembered position.
- Turning the encoder moves the highlight up/down (with scrolling once past the visible 3 rows).
- Confirming (arcade button or encoder click) on a game entry: shows a **1-second per-game splash
  screen** (§4.3), then enters that game fresh (its state is reset to its initial/ready
  condition every time it's entered this way).
- Confirming on "Settings" opens the Settings submenu (§4.5). Confirming on "Instructions" opens
  the Instructions screen (§4.6).
- A small volume indicator icon is shown at the top-left of the menu screen at all times,
  reflecting the current volume level (§6) — same visual design as the icon described in §6.1.

### 4.3 Per-Game Splash Screen
Shown for exactly 1 second immediately after selecting a game from the menu, before that game's
own first screen appears:
- The selected game's icon, large, in the upper portion of the screen.
- The game's translated name below it, in the same pixel/arcade font used for the boot splash
  (§4.1, §9) — sized as large as fits on one line, automatically splitting onto two lines at a
  word boundary (never mid-word) if the translated name is too wide for one line at that size.

### 4.4 (see §3.4 — Leave-confirmation dialog is documented there since it's a global mechanism,
not menu-specific.)

### 4.5 Settings Submenu
A 3-row list (same visual style as the main menu): **Language**, **Timer Step**, **Config Mode**.

- **Language:** turning the encoder cycles through English, French ("Français"), Spanish
  ("Español") — each shown in its own native name regardless of the currently active language, so
  it's always readable. Confirming applies the choice immediately across the entire interface and
  saves it to flash.
- **Timer Step:** controls how much the Timer game's length changes per encoder detent while
  setting it up. Options: **Auto** (default — a "ramped" step that's fine-grained for short
  durations and coarser for long ones, shared with the Chess Clock's starting-time picker, see
  §5.1/§5.4 for the exact ramp), or a fixed step of **1, 5, 10, 30, or 60 seconds**. Saved to
  flash on confirm.
- **Config Mode:** immediately starts the WiFi access point and web server described in §7 — this
  entry does not open a further submenu, selecting it starts Config Mode right away. Exiting
  Config Mode (either physical button) returns here with this row still highlighted.

### 4.6 Instructions Screen
A single static screen (no interaction beyond closing it) listing the three global controls in
short form, translated:
- "Menu: hold knob 1s"
- "Volume: hold+turn"
- "More: see Settings"

Closed by either the arcade button or an encoder click, returning to the main menu with the
cursor on the Instructions entry.

---

## 5. Games

Every game screen follows a shared visual convention unless stated otherwise: a small icon at
top-left identifying the game, a status/header line right-aligned in the remaining top space
(this space used to also hold a persistent mute icon; that icon was removed and volume moved to
the menu screen instead — see §6), a horizontal divider line beneath that header row, and the
game's main content below the divider.

### 5.1 Timer
A simple countdown timer.
- **Idle:** turning the encoder adjusts the target duration, moved by the step chosen in
  Settings (§4.5). Default 2:00. Range 0:10–90:00 (`MIN_TIME`=10s, `MAX_TIME`=5400s,
  `DEFAULT_TIME`=120s). The **Auto** ramp step scales the increment based on the current value
  so it's easy to fine-tune short durations and quickly cover long ones — this same ramp
  function is reused by Chess Clock's starting-time picker (§5.4).
- Press the arcade button to (re)start the countdown from the currently set duration; a short
  "go" jingle plays.
- **While running:** the encoder no longer adjusts the time (turning it does nothing), so it
  can't be bumped by accident mid-countdown. The button's LED blinks the whole time, speeding up
  as time runs low. Displayed as `M:SS`, largest text size that fits.
- An encoder short click pauses/resumes while running (this game's one context-specific override
  of "click = confirm," since there's nothing to confirm mid-countdown). The LED and on-screen
  progress freeze while paused.
- A two-tone chime plays once at the halfway point.
- The buzzer beeps once per second for the final 5 seconds — no other early warning.
- At zero: LED goes solid on, a short end jingle plays, and the countdown holds at 0:00 until
  restarted.

### 5.2 Reflex
A reaction-time light/buzzer game.
- Press the arcade button to arm a round (short start jingle).
- After a random delay, the LED turns on with a sharp cue sound — press the button as fast as
  possible; reaction time in milliseconds is measured from the exact instant of the physical
  press (not when the dispatch logic gets around to processing it) and shown on screen.
- **False start:** pressing before the light comes on triggers a bomb-style explosion animation
  (identical animation/asset to Pass the Bomb's own explosion, §5.5) and shows "TOO SOON" in
  large text for a fixed display duration (`FALSE_START_DISPLAY_MS` = 1500ms), with no bomb
  icon shown afterward — just the text, purely a shared visual effect for the "you failed"
  moment, not a bomb-themed feature in this game.

### 5.3 Dice
Rolls 1–5 dice of a chosen type.
- **Selection screen:** turning the encoder cycles the die type (the available face counts,
  e.g. d4/d6/d8/d10/d12/d20 — implement whatever standard tabletop die set makes sense; the
  original build supports the common D&D-style set). Confirm with the arcade button (or encoder
  click) to move to choosing dice count.
- **Count screen:** turning the encoder sets how many dice to roll (1–5, `MAX_DICE_COUNT`=5).
  Confirm to roll.
- **Rolling:** a brief multi-frame animation cycles random faces before settling on the final
  result(s), which are then displayed until the next action.

### 5.4 Chess Clock
Two-player countdown clock with increment.
- **Setup (two steps):** first pick the starting time per player (range 0:30–90:00,
  `CHESS_START_MIN_SECONDS`=30, `CHESS_START_MAX_SECONDS`=5400, using the same Auto-ramp step
  logic as the Timer game), then pick the increment added per move (0–60 seconds, in 10-second
  steps). Confirm each with the arcade button.
- **Play:** the currently-active player's remaining time counts down; the whole screen inverts
  (or otherwise strongly indicates) which side is active. The arcade button "passes" the turn —
  stopping the active player's clock, adding the increment to the player who just moved, and
  starting the other player's clock.
- An encoder short click pauses/resumes the whole clock (this game's other context-specific
  click override, alongside Timer).
- When a player's clock reaches zero, the game ends and declares the other player the winner.

### 5.5 Pass the Bomb
A party "hot potato" game with a spoken category+letter challenge.
- On entering, a random combo is picked: one of **20 categories** (list below) paired with a
  random letter A–Z.
- **Category list** (each is a singular noun, phrased so it reads naturally as "*Category*
  starting with *Letter*"): Country, City, Food, Celebrity, Cartoon character, Thing at home,
  Thing at school, Thing you wear, Sport, Body part, Musical instrument, Red thing, Blue thing,
  Green thing, Cold thing, Warm thing, Thing that flies, Colour, Thing that's noisy, Thing that
  breaks. All 20 are fully translated (EN/FR/ES) and each can be individually enabled/disabled
  via Config Mode (§7.2), with the same "at least one must stay enabled" rule as everywhere else.
- **Ready screen:** shows the category+letter challenge, split across the screen — the category
  name occupies the left ~2/3 of the width, the word "Letter" plus the letter itself (shown
  large) occupies the right ~1/3.
- Turning the encoder **while in this ready state** rerolls the combo with a slot-machine-style
  animation: rapidly cycles through random combos (only drawing from *enabled* categories),
  decelerating over roughly a dozen frames before settling on a final pick, with a short tick
  sound per frame.
- Press the arcade button to arm the bomb: a random hidden timer between **10 and 60 seconds**
  starts. Deliberately, **there is no visual or audible countdown hint of any kind** while
  armed — the LED simply blinks at a fixed, unchanging rate the entire time (not tied to time
  remaining, so it can't be read as a clock), and no ticking sound plays. This is intentional —
  the whole point of the game is not knowing when it goes off.
- When the timer runs out: the same explosion animation shared with Reflex's false-start (§5.2)
  plays, then a "BOOM!" screen (text only, no bomb icon — matching the plain style of Reflex's
  "TOO SOON" screen). Press the arcade button to return to the ready screen with a freshly
  rolled combo.

### 5.6 Turn Order
Randomly picks a number to decide who goes first.
- Turning the encoder sets the number of players (2–10).
- Press the arcade button to spin; shows a result from 1 to the chosen player count.
- Changing the player count after a result invalidates that result (must re-spin).

### 5.7 Stopwatch
Standard count-up stopwatch with lap support.
- Press the arcade button to start/stop.
- An encoder click records a lap while running.
- Elapsed time is tracked precisely across pause/resume (accumulated milliseconds, not
  re-measured from a single start timestamp), so pausing and resuming never drifts.

### 5.8 Trivia
Buzzer-style multiple-choice trivia with a physical "buzz in" mechanic.
- **Setup:** pick a category (8 available: Sports, Geography, Science, History, General,
  Mythology, Art, Animals — `TRIVIA_REAL_CATEGORY_COUNT`=8) then a difficulty level (3 levels:
  Junior, Curious, Master — `TRIVIA_LEVEL_COUNT`=3), each via encoder-turn + confirm.
- **Content storage:** each category+level combination is a CSV file on the device's flash
  filesystem, named `<category>_<level>.csv` (lowercase, e.g. `sports_junior.csv`). Format: a
  header row followed by one row per question, **15 plain comma-separated columns, no quoting
  support** (values must not contain literal commas or newlines):
  `en_question,en_correct,en_wrong1,en_wrong2,en_wrong3,fr_question,fr_correct,fr_wrong1,fr_wrong2,fr_wrong3,es_question,es_correct,es_wrong1,es_wrong2,es_wrong3`
  — i.e. the full question is stored in all 3 languages simultaneously in every row, and only
  the columns matching the currently active language are shown to the player. Up to 3 wrong
  answers are supported per question; fewer are fine (some rows may have blank wrong-answer
  columns for questions with only 2 total choices).
- **Play — question order:** at the start of a round, the full set of questions for that
  category+level is shuffled once into a fixed random order for the session. Navigation moves
  through this fixed order, so revisiting a question always shows the exact same content, never
  re-randomized.
- **Play — navigation:** turning the encoder moves through a single linear sequence:
  Question(1) ↔ Answers(1) ↔ Question(2) ↔ Answers(2) ↔ Question(3)…, freely reversible in
  either direction (wraps at both ends of the shuffled set). Turning right/forward from a
  Question view goes to that same question's Answers view; turning right again from Answers
  advances to the *next* question's Question view. Turning left does the mirror image. The
  bottom of the screen shows left/right arrow hints with short translated labels describing
  what each direction does from the current view (e.g. "Prev"/"Ans" on a Question screen,
  "Ques"/"Next" on an Answers screen), separated by a vertical divider.
- **Buzzing in:** pressing the arcade button reveals which of the shown options is correct
  (highlighted) and starts a ~5-second visible/audible countdown window during which the reveal
  is shown; it auto-hides after that window. This is independent from moving between
  Question/Answers views — you can look at the answer choices without buzzing in.
- Answers are displayed as a shuffled list of up to 4 options (the correct one plus its wrong
  answers, order randomized per question) each time a question is shown.

### 5.9 Toothbrush Timer
A 4-quadrant brushing timer (upper-left, upper-right, lower-left, lower-right — the four
quadrants of a mouth).
- Select duration (1–5 minutes) via encoder, confirm to start.
- While running, the screen divides the total time into 4 equal segments, one per quadrant, and
  advances through them in sequence, playing a short beep on each quadrant change so the user
  knows to move the brush without needing to watch the screen.
- An encoder click pauses/resumes.
- On completion, plays the same celebratory "firework" animation used elsewhere for
  positive milestones (e.g. Reflex's new-personal-best moment), rather than a bespoke animation.

### 5.10 Tetris
A compact falling-block game, played with the whole device rotated 90°.
- **Display orientation:** the screen is temporarily rotated in software so the physical panel's
  long 128px axis becomes the vertical "falling" axis and the short 64px axis becomes the
  board's width. The board is **8 columns × 15 rows**, 8px square cells (`TETRIS_BOARD_COLS`=8,
  `TETRIS_BOARD_ROWS`=15) — one row shorter than a full 16 to leave an 8px-tall HUD strip
  (`TETRIS_HUD_HEIGHT`=8) at the top showing `S<score> L<lines>`.
- **Pieces:** the standard 7 tetrominoes (I, O, T, S, Z, J, L), each with up to 4 rotation
  states (I/S/Z only have 2 visually distinct states; their other two rotations repeat the same
  two shapes rather than locking up when rotated). New piece type chosen uniformly at random on
  each spawn, centered at the top of the board; if a freshly spawned piece immediately overlaps
  existing blocks, the game ends.
- **Controls:** turning the encoder moves the falling piece left/right. Encoder click rotates it
  clockwise one step (silently, no sound effect). The arcade button **held down** speeds up the
  drop (soft drop); the normal fall speed itself also gradually increases every 10 lines
  cleared, down to a minimum interval floor.
- **Line clears:** when one or more rows fully fill, they flash (toggle filled/empty a couple of
  times) before being removed and everything above shifting down. Scoring follows the classic
  40/100/300/1200-points-per-simultaneous-clear table, multiplied by `(current level + 1)` where
  level = lines-cleared ÷ 10.
- **Game over:** the board fills solid from the bottom row upward (a quick visual "topping out"
  cascade), then flashes a few times with a descending 3-note cue, before showing the final
  score.
- Both the Ready and Game Over screens show an inverted title bar at the top ("TETRIS" /
  translated "Game Over"), which automatically wraps to two lines if a translation is too wide
  for the rotated 64px width (this matters for at least French and Spanish "Game Over").

### 5.11 Star Drop
A falling-catch game, also played rotated 90° (same rotation approach as Tetris).
- **Board:** 4 vertical lanes, 16px each (`STARDROP_LANES`=4, `STARDROP_LANE_WIDTH`=16, filling
  the 64px rotated width exactly), with an 8px HUD strip at the top
  (`STARDROP_HUD_HEIGHT`=8) showing `S<score> x<lives>`.
- **Objects:** stars, planets, and moons fall from the top in random lanes. Weighted spawn:
  60% star (worth 10 points), 30% planet (worth 20), 10% moon (worth 30) — rarer objects are
  worth more. Up to 6 objects can be falling simultaneously (`STARDROP_MAX_OBJECTS`=6).
- **Controls:** turning the encoder moves a "telescope" catcher between the 4 lanes. No other
  control is used during play.
- **Catching:** when a falling object reaches the catcher's row (`STARDROP_CATCHER_Y`=116, near
  the bottom of the 128px rotated height) it's resolved instantly: if the catcher is in the same
  lane, it's caught (points awarded); otherwise it's a miss and one of 3 starting lives is lost
  (`STARDROP_START_LIVES`=3). Zero lives ends the round.
- **Difficulty ramp:** both fall speed and spawn frequency increase gradually with catches
  (every 8 catches = one "level"): fall interval starts at 95ms/pixel-step and decreases 6ms per
  level down to a 30ms floor; spawn interval starts at 1400ms and decreases 50ms per level down
  to a 650ms floor.
- **High score:** persisted to flash (`sdHigh`), shown on both the Ready and Game Over screens,
  updated immediately whenever beaten.
- **Game over:** a few quick full-screen black/white flashes with a descending 3-note cue (a
  simpler variant of Tetris's game-over animation, since there's no board to visually "fill").
- Same inverted-title-bar treatment and translation-safe wrapping as Tetris (§5.10) on both the
  Ready ("STAR DROP" — kept identical across all 3 languages, it's treated as a proper noun) and
  Game Over screens.

---

## 6. Volume Indicator System

Two distinct renderings of the same "speaker + sound waves" icon concept, sharing one design
language: a small rounded box seamlessly merged with a steep, rounded-corner triangular flare
(no visible seam between the two shapes), pointed in whichever direction suits its placement.

### 6.1 Persistent Small Icon (main menu, top-left)
Always visible on the main menu screen (not shown inside individual games — see below). Shows:
- Mute: an "x" mark.
- Low: one close-parenthesis-style wave mark.
- High: two wave marks.

### 6.2 Full-Screen Overlay (shown live while adjusting)
Triggered exactly by the "hold encoder + turn" gesture (§3.2), from any screen. A large-scale
version of the same speaker glyph, filling most of the screen, with correspondingly large wave
marks (drawn as short connected line-segment arcs, doubled up at adjacent radii to read as a
clearly thick stroke rather than a hairline — the display library has no native arc-drawing
primitive) or a large X for mute. Updates live with every encoder detent while held; the
previous screen is restored the instant the encoder is released.

Note: an earlier design iteration also showed a small mute/volume icon at the top-right of every
individual game screen. That was removed — volume is now only shown persistently on the menu
screen (§6.1) plus the full-screen overlay while actively adjusting (§6.2) — freeing that
top-right space so each game's header status text could be right-aligned into it instead (§5,
intro paragraph).

---

## 7. Config Mode (WiFi Web Configuration Tool)

### 7.1 Activation and Network
- Entered only via Settings → Config Mode (§4.5) — never active during normal gameplay, so it
  carries zero battery cost outside of deliberate use.
- Starts the ESP32 as its own WiFi access point, SSID **"TigerTimer-Config"**, **open network
  (no password)** — chosen deliberately so phones can auto-join via a captive-portal prompt
  without any manual entry.
- Runs a DNS server that answers every lookup with the device's own IP, plus an HTTP redirect on
  any unrecognized path — together these make most phones automatically pop up a "Sign in to
  network" prompt straight to the config page on connecting, without needing to type an IP
  address in manually (falls back gracefully to manual entry at `192.168.4.1` if a given
  phone/OS doesn't auto-prompt).
- The on-device screen while Config Mode is active shows a QR code (encoding the standard
  `WIFI:T:nopass;S:TigerTimer-Config;;` join-network format, scannable by any phone camera app)
  plus the SSID and IP as text fallback.
- Exits back to the Settings screen on either physical button.

### 7.2 Web Page Feature Set
A single-page web app served from the device itself (no internet connection needed or used —
the whole page, including its JS/CSS, is self-hosted on the ESP32's flash), with these sections:

1. **Menu order & games** — every menu entry (games plus the Instructions/Settings sentinels)
   listed with up/down arrow buttons to reorder (chosen deliberately over drag-and-drop, which
   proved unreliable specifically in mobile browsers), and a toggle switch per *game* (not
   shown for Instructions/Settings, which can never be disabled) to include/exclude it from the
   on-device menu — enforcing the "at least one game must stay enabled" rule (§3.5).
2. **Startup** — choose "go to menu" vs. "start a specific game" on boot, with a dropdown of
   currently-enabled games for the latter.
3. **Default timer length** — a numeric seconds field with a live M:SS preview, for the Timer
   game's default starting value.
4. **Trivia questions** — pick a category and level, then edit each question's full 15-field
   content in an expandable per-question editor (English/French/Spanish question + correct
   answer + up to 3 wrong answers each). Includes:
   - **Download CSV** — downloads the *currently saved on-device* CSV for that category/level as
     a real HTTP file download (`Content-Disposition: attachment`), deliberately *not* using a
     JS Blob-URL technique, because that approach is known to silently fail in some restricted
     mobile browser contexts (notably Android's captive-portal sign-in WebView, which this
     whole feature is likely to be accessed through — see §7.1).
   - **Upload CSV** — loads a CSV file into the in-browser editor for review (does not
     auto-save; the user must still hit Save to write it to the device).
   - **Add question** / **Save questions** to append a blank row or persist edits back to the
     device's flash file for that category/level.
5. **Pass the Bomb categories** — all 20 categories (§5.5) listed with their English name and
   French/Spanish translations shown beneath each, and a toggle per category (same
   "at least one enabled" rule).

### 7.3 API Surface (for reference — a from-scratch reimplementation doesn't need to match these
exact routes, only the equivalent capability)
- `GET /` — serves the config page itself.
- `GET /api/config` — returns current menu order, per-game enabled state, startup settings,
  default timer length, trivia category/level lists, and the full Pass the Bomb category list
  (with translations and enabled state) as JSON.
- `POST /api/config` — accepts a partial update to any of the above; applies and persists
  immediately; enforces the "at least one enabled" rule per toggle-able group independently,
  reporting a non-fatal warning in the response if any part of a save was rejected for that
  reason while still applying the rest.
- `GET /api/trivia?category=..&level=..` — returns that CSV's question rows as JSON.
- `POST /api/trivia?category=..&level=..` — rewrites that CSV file from submitted JSON rows.
- `GET /api/trivia/csv?category=..&level=..` — streams the raw CSV file as a real file download
  (see the Download CSV rationale above).

---

## 8. Localization System

- Every user-facing string lives in a single string table indexed by an integer id, with one
  column per language (English, French, Spanish) — roughly 120 strings total across the whole
  interface.
- A `T(id)` lookup function returns the string in whichever language is currently active
  (`lang` setting, §3.5).
- A handful of names are intentionally **not** translated and appear identical in all 3 language
  columns, being treated as proper nouns/brand names: "Tetris", "Star Drop".
- **Word-wrapping is always word-boundary-aware, never mid-word** — this matters because
  translated strings vary substantially in length (e.g. Spanish "Temporizador" vs English
  "Timer"), and several screens (splash screens, Tetris/Star Drop's inverted title bars, Pass
  the Bomb's challenge text) must gracefully wrap onto multiple lines rather than truncating or
  breaking a word across the line boundary.
- Implementation note for whoever builds this: if using a display library with its own built-in
  automatic text-wrapping, be aware that measuring text width *while that automatic wrap is
  still enabled* can return a size that already reflects the *wrapped* (and therefore
  potentially mid-word-broken) layout rather than the string's true single-line width — leading
  to exactly the bug described (word-wrap decisions silently going wrong). Disable any built-in
  automatic wrap before measuring/positioning custom-wrapped text, and do the line-breaking
  decision entirely in your own code.

---

## 9. Custom Display Font

A separate pixel/arcade-style bitmap font (visually matching the boot splash's "TIGER"/"TIMER"
wordmark, §4.1) is used specifically for the per-game splash screens (§4.3) and the Tetris/Star
Drop title bars (§5.10/§5.11), layered alongside the display library's normal built-in font used
for everything else. It's an 8×8-pixel-per-character monospace bitmap font, covering standard
ASCII plus the two accented characters needed by the French translations (é, è) at the same code
points the rest of the firmware already uses for those characters. If reproducing this: any
freely-licensed 8×8 "retro arcade" pixel font converted to a bitmap font format compatible with
your display library will achieve the same visual effect; exact glyph-for-glyph reproduction of
the original isn't functionally necessary, only the general blocky/pixelated style.

---

## 10. Known Limits of This Document

Being honest about where prose specification reaches its limit, per the earlier note at the top:

- **Exact pixel coordinates** for every element on every screen (icon positions, text baselines,
  divider lines, column widths, etc.) are described functionally here (e.g. "right-aligned in
  the header," "left ~2/3 / right ~1/3 split") but not enumerated pixel-by-pixel for all ~20
  distinct screens. A rebuild from this document alone will be behaviorally equivalent but is
  very unlikely to be visually pixel-identical.
- **The exact tetromino rotation-state bitmask table** (§5.10) is not reproduced numerically
  here — "the standard 7 tetrominoes with up to 4 rotations each" is sufficient for a correct
  reimplementation, since this is a well-known, well-documented game mechanic independent of
  this specific project.
- **The full trivia question content** (the actual CSV data files) is not part of this
  specification at all — it's game *content*, not game *behavior*, and ships as separate data
  files.
- **The custom font's exact glyph bitmaps** (§9) are not reproduced here as data.

For anything in this list, the actual source files (firmware `.ino`/`.h`/`.c` files, the
`data/config.html` web page, and the trivia CSV data files) are the authoritative reference —
see the companion development plan for how those fit into a staged rebuild.
