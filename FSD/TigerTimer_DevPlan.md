# Tiger Timer — Development Plan

**How to use this document:** Each task below is scoped to be small enough for a single focused
coding session (by a human or an AI model with limited context), and lists exactly which FSD
section(s) it implements, so the corresponding part of `TigerTimer_FSD.md` can be pulled in as
just-in-time context rather than needing the whole FSD loaded at once. Tasks within a phase are
ordered by dependency; phases are ordered so that every phase only depends on earlier ones.
**Do not start a task without first re-reading its "FSD section(s)" — that's the actual spec;
this plan only sequences the work.**

Each task has a **Definition of Done** — a concrete, checkable condition, not just "implement
the feature." Where a task says "stub," it means: build the minimum that lets later tasks
compile and run against it, with a clearly marked TODO, not a fully finished feature.

---

## Phase 0 — Project Scaffolding

**0.1 — Repository/sketch skeleton**
- Create the Arduino sketch folder structure. One primary `.ino` file plus (as the project
  grows) additional `.ino` "tab" files in the same folder — Arduino concatenates every `.ino` in
  a sketch folder into one compile unit, so this is purely an organizational split, not a real
  module boundary. A `data/` subfolder holds anything uploaded to the device's filesystem
  (trivia CSVs). The config web page lives in `web/config.html` and is embedded into the firmware
  (`tools/embed_config_html.py` → `config_html.h`), so a firmware update also updates it.
- FSD section(s): §2.1.
- Definition of Done: sketch compiles empty (just `setup(){}`/`loop(){}`) targeting the XIAO
  ESP32C6 board profile in the Arduino IDE, with the Adafruit SH110X library installed.

**0.2 — Pin and hardware constant definitions**
- Define every pin/hardware constant from §2 as named constants at the top of the main file
  (button, button LED, buzzer, encoder ×3, OLED address/dimensions/I2C pins).
- FSD section(s): §2.2–§2.5.
- Definition of Done: constants exist and match §2 exactly; nothing else references them yet.

---

## Phase 1 — Core Drivers

**1.1 — Display bring-up**
- Initialize the SH110X display over I2C; confirm you can clear the screen and draw a test
  rectangle/text on boot.
- FSD section(s): §2.2.
- Definition of Done: something visibly renders on the physical (or simulated) OLED at boot.

**1.2 — Debounced button reader**
- Implement a debounced reader for the arcade button (§2.3) that distinguishes "just pressed"
  and "just released" edges, exposes the current raw held/not-held state and the exact
  press-start timestamp for other code to use (needed later by both Reflex and SimpleMorse), and
  drives the button LED as a simple `digitalWrite` on/off (blink logic comes later, per-game).
- FSD section(s): §2.3.
- Definition of Done: a short press reliably logs exactly one "pressed" and one "released" event
  with no duplicate/missed events across ~20 manual test presses, and the logged press-start
  timestamp matches a stopwatch-measured hold duration to within debounce tolerance.

**1.3 — Debounced encoder reader (rotation + button)**
- Implement rotation direction detection (CLK/DT reading, §2.4) and a separately-debounced
  reader for the encoder's own push button, exposing: rotation direction events, "just pressed,"
  "just released," and continuous held-state.
- FSD section(s): §2.4.
- Definition of Done: turning the encoder one detent in each direction logs exactly one `+1` or
  `-1` event; encoder button press/release behaves like 1.2's button reader.

**1.4 — Buzzer driver**
- Set up the LEDC PWM channel on the buzzer pin; implement a blocking `playNote(frequency,
  durationMs)` helper, and separate non-blocking `startTone(frequency)` / `stopTone()` helpers
  for tones that need to persist across many loop iterations (needed later by SimpleMorse). All
  three should share the same volume-driven duty-cycle logic. Implement a volume state (0/1/2)
  that maps to PWM duty per §2.5. Muted (volume 0) must make `playNote()` skip the call entirely
  rather than playing silently, so it doesn't consume the note's duration; `startTone()` should
  behave the same way, and `stopTone()` must always be safe to call even if nothing is playing.
- FSD section(s): §2.5.
- Definition of Done: calling `playNote()` at each of the 3 volume levels produces audibly
  different loudness (or is at minimum wired correctly per the duty values in §2.5 if hardware
  isn't available for this task); `startTone()` followed by a delay followed by `stopTone()`
  produces a tone of exactly that duration; calling `stopTone()` with nothing playing does
  nothing harmful.

**1.5 — Deep sleep + wake**
- Implement the deep-sleep entry function per §2.6/§3.3 (RTC peripheral kept on, EXT1 wake on
  the button pin, `esp_deep_sleep_start()`).
- FSD section(s): §2.6, §3.3.
- Definition of Done: calling the deep-sleep function puts the chip to sleep, and pressing the
  arcade button wakes it (full reboot is expected and correct).

---

## Phase 2 — Persistence Layer

**2.1 — Preferences wrapper**
- Wrap the Arduino `Preferences` library for the single namespace and all keys listed in §3.5,
  with typed getters/setters and the documented defaults for each key (including the three
  per-game high score keys, even though no game exists yet to populate them). Don't wire these
  into any UI yet — just the storage layer.
- FSD section(s): §3.5.
- Definition of Done: a value written for each key and re-read after a simulated reboot
  (re-opening the Preferences namespace) returns the same value; missing keys return the
  documented default.

---

## Phase 3 — Localization

**3.1 — String table + language switch**
- Build the string-table system: an enum/index of string ids, a 3-column (EN/FR/ES) data table,
  and a `T(id)` lookup that reads the current language from the persistence layer (2.1). Seed it
  with a handful of placeholder strings to prove the mechanism; the full string set gets filled
  in incrementally as each later feature needs its own strings.
- **Critical, learned from a real shipped bug:** the table is a pure positional lookup — array
  index must always equal the string's declared id. If you ever add strings by inserting a row
  near a related existing entry (for readability) while assigning its id sequentially at the
  end, every entry from that point on will silently return the wrong language. Either enforce
  strict append-only insertion, or build a small verification script now that checks every
  declared id against its row's actual array position, and re-run it after every string-table
  edit for the rest of the project.
- FSD section(s): §8.
- Definition of Done: calling `T(id)` after switching the stored language value returns the
  correct column; an id with no data for a language falls back sensibly (document your choice —
  e.g. fall back to English) rather than crashing or returning garbage; the verification
  script (if you built one) passes on the current table.

**3.2 — Word-boundary text wrapping helper**
- Implement a reusable "wrap this string to fit N pixels wide, breaking only at spaces, up to a
  given max line count" function, and its screen-width-independent variant for narrower columns.
  **Read the implementation gotcha in FSD §8 carefully** (disable any display-library
  auto-wrap before measuring/positioning) before writing this.
- FSD section(s): §8, and the note at the end of §8 specifically.
- Definition of Done: a string wider than the target width wraps at a space, never mid-word,
  verified with at least one deliberately long test string per supported language.

---

## Phase 4 — Menu System & Global Input Model

This phase is the architectural backbone everything else plugs into — get it right before
building any individual game.

**4.1 — Screen/state model**
- Define the top-level state machine: which "mode" the device is in (main menu / settings
  submenu / language picker / timer-step picker / instructions / leave-confirm dialog / inside
  game N). Only a skeleton is needed here — enough to route input and rendering to the right
  place — individual game logic comes in later phases.
- FSD section(s): §3.2 (for what states must exist), §4 (menu structure).
- Definition of Done: the device can be in exactly one state at a time, state transitions are
  centralized (not scattered ad-hoc), and there's a render dispatcher that draws the right thing
  for the current state (even if most states just draw a placeholder for now).

**4.2 — Global input dispatcher**
- Implement the full input model from §3.2's table: short click vs. 1-second long-press vs.
  hold+turn disambiguation on the encoder, and the arcade button's short-press dispatch. This is
  the trickiest piece of the whole project to get right — pay special attention to the mutual
  exclusivity rules (a hold claimed by volume never also fires a click or the long-press menu;
  whichever condition is met first wins), to §3.6's phantom-click note (which applies to this
  same debounce machinery), and to the Settings-list-specific long-press exception (jumps
  straight to the menu with no confirmation, unlike every other in-game long-press).
- FSD section(s): §3.2, §3.6, §4.5 (the Settings long-press exception specifically).
- Definition of Done: manually test every row of the §3.2 table in isolation (a short click, a
  long-press with no rotation, a hold+turn, a hold+turn followed by continued turning after the
  long-press fires, and a long-press from the Settings list specifically) and confirm each
  produces exactly the documented behavior with no cross-triggering.

**4.3 — Main menu list + scrolling**
- Implement the 3-row scrollable menu list, default order (§4.2), icon+label rendering,
  highlight-inverts-the-row rendering, and confirm-to-select. Games themselves don't exist yet —
  confirming an entry can just log/stub "entered game N" for now.
- FSD section(s): §4.2.
- Definition of Done: all default-order entries are reachable by scrolling in both directions,
  wrapping behavior at the list ends matches your chosen convention (document it), and the
  cursor always starts on the first entry after a fresh boot.

**4.4 — Settings submenu (Language + Timer Step + Remote Config entry)**
- Build the 3-row Settings list and the Language/Timer Step picker screens, wired to the
  persistence layer (2.1) and string table (3.1). The Remote Config row can stub-log "would
  start Remote Config here" until Phase 12.
- FSD section(s): §4.5.
- Definition of Done: changing language and confirming immediately re-renders the settings list
  itself in the new language (proves the live-switch works, not just storage); long-pressing the
  encoder from this list returns to the main menu immediately, with no dialog.

**4.5 — Instructions screen**
- Static screen per §4.6, using the string table.
- FSD section(s): §4.6.
- Definition of Done: opens from the menu, shows the 3 lines, closes back to the menu via either
  control.

**4.6 — Leave-confirmation dialog**
- Build the Yes/No dialog exactly per §3.4, wired to the global input dispatcher's long-press
  gesture from a stub "inside a game" state (a placeholder game state is fine here — real game
  teardown logic gets filled in as each game is built).
- FSD section(s): §3.4.
- Definition of Done: from the stub game state, long-press opens the dialog defaulted to "Yes";
  turning toggles the highlight; confirming "No" returns to the stub game unchanged; confirming
  "Yes" returns to the menu with the cursor on the game just left.

**4.7 — Per-game splash screen**
- The 1-second icon+name splash shown when entering any game from the menu (FSD §4.3 - note
  this is FSD §4.3, distinct from this plan's own numbering).
- FSD section(s): FSD §4.3.
- Definition of Done: selecting any menu entry shows the splash for ~1 second (verify it's not
  skippable/interruptible, matching the spec) before the target screen appears.

**4.8 — Boot splash**
- The two-phase TIGER/TIMER slide-in animation, skippable by holding the arcade button, with
  the phantom-click-safe skip handling from §3.6.
- FSD section(s): FSD §4.1, §3.6.
- Definition of Done: normal boot plays the full animation; holding the button through any part
  of it skips straight to the post-splash startup logic with no phantom menu-select afterward.

At the end of Phase 4, you have a fully navigable menu shell with no actual games yet — this is
a good integration checkpoint.

---

## Phase 5 — Simple Games

Build these first since they're the least architecturally novel — they mostly exercise the
Phase 4 scaffolding rather than adding new mechanisms.

**5.1 — Timer game**
- FSD section(s): §5.1 (also needs the Auto-ramp step function, which Chess Clock will reuse in
  Phase 6 — write it as a standalone reusable function now).
- Definition of Done: every bullet in §5.1 is independently verifiable — idle adjustment
  respects the chosen step, running locks out adjustment, pause/resume via encoder click works,
  halfway chime fires once, final-5-seconds beeping, zero behavior.

**5.2 — Stopwatch game**
- FSD section(s): §5.7.
- Definition of Done: start/stop via button, lap via encoder click, elapsed time survives at
  least 3 pause/resume cycles without drifting (verify against a real stopwatch).

**5.3 — Turn Order game**
- FSD section(s): §5.6.
- Definition of Done: player count adjustable 2–10, spin produces a result in range, changing
  count after a spin clears the stale result.

**5.4 — Dice game**
- FSD section(s): §5.3.
- Definition of Done: die-type and count both selectable and confirmed independently, roll
  animation plays, final result(s) match the chosen die type's valid range.

---

## Phase 6 — Medium Games

**6.1 — Reflex game**
- FSD section(s): §5.2. Needs a shared "explosion animation" asset — build it here as a
  standalone reusable function, since Pass the Bomb (Phase 7) reuses the exact same one.
- Definition of Done: armed → random delay → light+sound cue → reaction time measured from the
  true press instant (not dispatch time) and displayed; pressing before the cue triggers the
  false-start path with the explosion animation and "TOO SOON" text, no bomb icon.

**6.2 — Chess Clock game**
- FSD section(s): §5.4. Reuses 5.1's Auto-ramp step function for the starting-time picker.
- Definition of Done: two-step setup, pass-turn via button (stops active clock + applies
  increment + starts other side), pause/resume via encoder click, zero-time end condition
  declares the correct winner, header shows the game's name before the clock has started and
  the turn indicator once it has.

**6.3 — Toothbrush Timer game**
- FSD section(s): §5.9. Build the celebratory "firework" animation here as a standalone reusable
  function — SimpleMorse (Phase 11) reuses this exact same one for its win screen.
- Definition of Done: 1–5 minute selection, 4 equal quadrant segments with a beep on each
  transition, header shows "Brush!" throughout the run, pause/resume, completion animation plays
  once.

---

## Phase 7 — Pass the Bomb

Broken out as its own phase since it has real content (20 categories) and several mechanics not
shared with anything built so far.

**7.1 — Category data + enable/disable storage**
- Define the 20 categories from §5.5 with their EN/FR/ES translations in the string table
  (Phase 3), and a persisted enabled/disabled flag array (extend Phase 2's persistence layer)
  with the "at least one enabled" guard rule from §3.5.
- FSD section(s): §5.5 (category list), §3.5 (persistence + guard rule).
- Definition of Done: all 20 categories are individually toggle-able in isolation (a temporary
  debug UI or direct function calls are fine for this task — the real web UI comes in Phase 12);
  attempting to disable the last remaining one is rejected.

**7.2 — Ready screen, layout, and reroll**
- The category+letter combo, the left-2/3/right-1/3 layout (category name vertically centered
  regardless of 1 vs 2 line wrapping), the bottom reroll-hint row (14px inverted band, rotate
  icon on the right near the physical encoder, hint text to its left, shown only while
  not-yet-armed), and the slot-machine reroll animation triggered by encoder rotation. The very
  first combo shown on entering the game (not just manual rerolls) should also play this
  animation.
- FSD section(s): §5.5.
- Definition of Done: reroll only ever lands on enabled categories; the animation visibly
  decelerates rather than looking like a single instant swap; the very first combo on entry
  plays the same animation; category text stays vertically centered whether it wraps to 1 or 2
  lines.

**7.3 — Armed/explode/auto-reshuffle logic**
- The hidden 10–60s timer, the deliberately-blank "no hint" behavior, the fixed-rate LED blink,
  volume forced to maximum on entering the game (once per session, not per arm), the explosion →
  "BOOM!" flow reusing 6.1's shared animation, and the 1-second auto-clear that reshuffles into a
  fresh combo (with the same reroll animation) — skippable early by turning the encoder during
  that pause.
- FSD section(s): §5.5.
- Definition of Done: across several armed rounds, the LED blink rate is visibly constant
  regardless of how close the hidden timer is to firing (this is the thing most likely to get
  implemented wrong by "helpfully" adding a speed-up — explicitly verify it does NOT speed up);
  the 1-second post-explosion pause reliably auto-advances; turning the encoder during that pause
  skips straight to the reshuffle.

---

## Phase 8 — Trivia

**8.1 — CSV format + LittleFS loading**
- Implement the 15-column CSV parser/writer per §5.8's exact format, and the filesystem
  mount/format-on-first-boot logic.
- FSD section(s): §5.8 (content storage), §3.1 step 3.
- Definition of Done: a hand-written test CSV with a couple of rows loads correctly into
  question/answer data structures; malformed rows (wrong column count) fail gracefully rather
  than crashing.

**8.2 — Category/level selection + shuffle**
- Selection screens (each showing the game's name in the header, since there's no other status
  text on these two screens), then the once-per-round shuffle of that file's questions into a
  fixed session order.
- FSD section(s): §5.8.
- Definition of Done: re-entering the same question index within a round always returns
  identical content; starting a new round re-shuffles.

**8.3 — Navigation (Question↔Answers↔next Question)**
- The bidirectional single-sequence navigation and bottom arrow-hint row.
- FSD section(s): §5.8.
- Definition of Done: every transition in the §5.8 navigation description is independently
  verified (both directions, from both view types, at the wrap points).

**8.4 — Buzz-in mechanic**
- Reveal-correct-answer-on-button-press with the ~5s auto-hide window, independent of navigation.
- FSD section(s): §5.8.
- Definition of Done: buzzing in doesn't advance navigation; navigating away and back doesn't
  get stuck in a "still revealed" state incorrectly.

---

## Phase 9 — Tetris

The first game needing the 90°-rotated-display technique — treat this as a new sub-skill to
verify carefully before Star Drop reuses it.

**9.1 — Rotated rendering + board model**
- The 8×15 board, temporary display rotation scoped tightly to this game's own draw calls (must
  never leak into any other screen), and the HUD strip.
- FSD section(s): §5.10 (display orientation + board dimensions).
- Definition of Done: after leaving Tetris, every other screen renders in normal orientation —
  explicitly test this, since a rotation "leak" bug is easy to introduce and easy to miss.

**9.2 — Piece system**
- All 7 tetrominoes, up to 4 rotation states each, spawn/collision/game-over-on-spawn-overlap
  logic.
- FSD section(s): §5.10.
- Definition of Done: all 7 piece types are reachable, all rotations of each are visually valid
  tetromino shapes, collision correctly blocks illegal moves at board edges and against locked
  cells.

**9.3 — Controls (move/rotate/hard-drop) + gravity/speed ramp**
- Encoder turn moves left/right; encoder click hard-drops (a single discrete action, not a held
  gesture, since the encoder's hold gesture is already claimed by volume-adjust and the
  menu-long-press — see §3.2); arcade button rotates. Normal fall speed increases every 10 lines.
- FSD section(s): §5.10, §3.2 (why hard-drop is a click, not a hold).
- Definition of Done: encoder moves left/right respecting collision, encoder click instantly
  drops and locks the piece, arcade button rotates, normal fall speed increases every 10 lines
  per the documented floor.

**9.4 — Ghost piece**
- Compute the row the current piece would land on if hard-dropped right now, by reusing the same
  collision-checking loop the hard-drop itself uses, without actually moving or locking anything.
  Draw it as an outline before the real (filled) piece each frame.
- FSD section(s): §5.10 (ghost piece).
- Definition of Done: the outline always matches exactly where a hard-drop would actually land,
  verified by comparing the ghost's shown position against the result of actually hard-dropping,
  for several different columns/rotations; the outline updates immediately when the piece is
  moved or rotated.

**9.5 — Line clear + scoring + game over + high score**
- The flash-before-remove animation, the scoring table, the topping-out game-over sequence, and
  persisting the high score to flash (only updating it when beaten).
- FSD section(s): §5.10.
- Definition of Done: single and multi-line clears score per the documented table × level
  multiplier; the flash is visibly a toggle, not instant; game over plays the full described
  sequence; the high score persists across a simulated reboot and only updates when beaten.

**9.6 — Ready/Game Over screens + inverted title bar**
- Including the word-boundary-safe two-line wrap for translations too wide for the rotated
  64px width, and showing the high score on both screens.
- FSD section(s): §5.10, §8 (wrap rule).
- Definition of Done: French/Spanish "Game Over" (both notably longer than English) wrap cleanly
  onto two lines rather than overflowing or breaking mid-word; high score is visible and correct
  on both screens.

---

## Phase 10 — Star Drop

Reuses Phase 9's rotation technique directly — should be noticeably faster to build than Tetris.

**10.1 — Lanes, catcher, object pool**
- FSD section(s): §5.11.
- Definition of Done: catcher moves cleanly between all 4 lanes; up to 6 objects can be on
  screen simultaneously without visual or logic glitches.

**10.2 — Spawning, weighting, catching, lives**
- FSD section(s): §5.11.
- Definition of Done: over many spawns, the observed type distribution is roughly 60/30/10;
  catches/misses resolve correctly at the catcher row; 3 misses end the round.

**10.3 — Difficulty ramp + high score**
- FSD section(s): §5.11.
- Definition of Done: fall/spawn intervals move per the documented per-level deltas and respect
  their floors; high score persists across a simulated reboot and only updates when beaten.

**10.4 — Ready/Game Over screens**
- Same inverted-title-bar + wrap treatment as Tetris.
- FSD section(s): §5.11, §8.
- Definition of Done: matches 9.6's acceptance criteria, applied to Star Drop's own screens.

---

## Phase 11 — Snake

Deliberately built in normal (unrotated) orientation, unlike Tetris/Star Drop — no rotation
sub-skill needed here.

**11.1 — Board, body, food**
- 32×12 grid, 4px cells, a fixed-size array-based snake body (cap it well under the board's
  total cell count), single-item food spawning on any unoccupied cell.
- FSD section(s): §5.12 (board).
- Definition of Done: the snake renders correctly as a chain of filled cells; food never spawns
  on an occupied cell.

**11.2 — Relative turning + turn-rate limiting**
- Encoder rotation turns the heading ±90° relative to the current direction (not absolute).
  Explicitly cap turn acceptance to once per movement tick — read the FSD's note on why a
  fixed-time debounce doesn't fully solve this (two rapid turns before the next move can still
  add up to a reversal at low speeds where the tick is slow, or feel laggy at high speeds where
  the tick is fast) — the cap must be tied to the game's own current tick interval, not a
  separate fixed constant.
- FSD section(s): §5.12 (controls, turn-rate limiting).
- Definition of Done: two rapid opposite-then-same turns issued before the next move never
  results in an unintended reversal, tested at both the slowest and fastest difficulty levels;
  a single turn is always accepted immediately (no perceptible lag) at any speed.

**11.3 — Wraparound movement + collision**
- Moving off any edge wraps to the opposite side. Self-collision (and only self-collision) ends
  the game.
- FSD section(s): §5.12 (walls do not kill).
- Definition of Done: deliberately drive the snake off each of the 4 edges and confirm it
  reappears correctly on the opposite side each time with no game-over; deliberately collide with
  the snake's own body and confirm that, and only that, ends the game.

**11.4 — Difficulty ramp, pause/resume, high score**
- Arcade button starts/pauses/resumes (not the encoder click, which is fully occupied by
  turning). Move interval ramps from 220ms to an 80ms floor. High score persists to flash.
- FSD section(s): §5.12.
- Definition of Done: pausing mid-game and resuming doesn't cause a missed-tick jump; speed
  ramp matches the documented per-level deltas; high score persists across a simulated reboot
  and only updates when beaten.

**11.5 — Ready/Game Over screens**
- Standard icon+header+divider template (not Tetris/Star Drop's rotated style).
- FSD section(s): §5.12.
- Definition of Done: high score shown on Ready; current score + high score both shown on Game
  Over.

---

## Phase 12 — SimpleMorse

The most mechanically novel game in the project — new input classification (press duration) and
a new live-audio-feedback pattern not used anywhere else.

**12.1 — Morse table + word pool + sequence flattening**
- Standard A-Z Morse code table. The 17-word pool from §5.13, each already verified (don't
  re-derive this by hand — write a small script/test that sums each word's total symbol count
  and confirms it's in the 10–15 range, since this is exactly the kind of arithmetic that's easy
  to get subtly wrong by hand). A function that picks a random word and flattens its full Morse
  code into one continuous dot/dash array (letter boundaries not preserved as gaps in the
  sequence itself, though see 12.3 for why you still need to track them separately).
- FSD section(s): §5.13 (word selection).
- Definition of Done: every word in the pool verified programmatically (not manually) to total
  10–15 symbols; the flattening function produces the correct sequence for several hand-checked
  words.

**12.2 — Ready screen: word + per-letter code display**
- Shows "Word in Morse" and the word with each letter's code beneath it, column width scaling to
  the word's length. This exact same rendering is reused later for the post-result solution
  reveal (12.6) — build it as a standalone, reusable function now.
- FSD section(s): §5.13 (Ready screen).
- Definition of Done: every word in the pool renders without overlapping columns (check the
  longest words with the widest individual letter codes specifically).

**12.3 — Playback**
- Blocking device turn: plays the first *N* symbols (N = current round) via LED + a fixed-pitch
  tone, dashes noticeably longer than dots, with gaps between symbols.
- FSD section(s): §5.13 (playback).
- Definition of Done: timing is audibly/visibly distinct between dots and dashes; playback
  length grows correctly as rounds advance.

**12.4 — Input classification + live tone feedback**
- Press-down starts a single constant-pitch tone and turns the LED on; release stops both and
  classifies the hold duration against the dash threshold. **Do not** switch the tone's pitch
  mid-hold based on crossing the threshold live — a real, hardware-observed bug came from doing
  exactly that (re-triggering the tone generator mid-tone to change pitch caused an audible pop
  on the piezo buzzer that read as a spurious extra beep). Duration alone, revealed only at
  release, is what distinguishes dot from dash — the live tone's job is only to confirm "a press
  is currently being registered," not to reveal the classification early.
- **Also critical, learned from a real shipped bug:** the tone-stop-on-release call must be
  unconditional, not gated behind the same state check used to decide whether to start the tone.
  Game logic between a press and its matching release can legitimately change the state (e.g.
  completing a round transitions into playback for the next one) — if both the start and stop
  are gated on the identical state check, the stop can end up silently skipped because the state
  changed in between, leaving a tone stuck on. Make `stopTone()` safe to call unconditionally on
  every release regardless of current state (it's a no-op if nothing's playing), and only gate
  the LED write and any other side effects that would be wrong to apply to a different game.
- FSD section(s): §5.13 (input), §2.5 (`startTone`/`stopTone` semantics).
- Definition of Done: holding for various durations classifies correctly against the 400ms
  threshold; the tone is audibly one constant pitch for the entire hold, never changing mid-hold;
  releasing always stops the tone immediately, tested specifically around a round-advance
  transition (release the very last correct press of a round and confirm the tone still stops
  cleanly even though state changes immediately afterward).

**12.5 — Per-letter progress boxes**
- Track, per letter, the cumulative sequence index at which that letter's code completes.
  During input, render one box per letter: outline with the letter shown normally inside until
  that letter's full code has been correctly repeated this round, then filled solid with the
  letter inverted. Never show the actual code during this phase.
- FSD section(s): §5.13 (per-letter progress display).
- Definition of Done: boxes fill in the correct order as a round is correctly repeated; a new
  round's playback always resets every box back to outline-only; the Morse code itself is never
  rendered anywhere during this state, only during Ready/solution screens.

**12.6 — Win/lose animations + solution reveal**
- Wrong input → shake-and-sad-tone animation settling on a wounded walrus face (design this
  visually before committing to code — a hollow-outline vs. filled-body approach won't work for
  a normal-vs-wounded expression change; go with distinct eye/tusk treatments instead). Complete
  the full sequence → reuse Toothbrush Timer's firework animation (Phase 6.3). Either result:
  hold the final animation frame for 1.5 seconds, then automatically switch to the same
  word+code solution display built in 12.2, which persists until the button starts a fresh round.
- FSD section(s): §5.13 (losing, winning).
- Definition of Done: both results hold their animation for measurably ~1.5s before switching;
  the solution screen shown afterward is pixel-identical in layout to the Ready screen's display
  (same reused function); starting a fresh round picks a new random word.

---

## Phase 13 — Volume System

Deliberately built after the games exist, since the full-screen overlay needs to correctly
suspend and restore whatever screen was active underneath it — easier to test with real games in
place. Also deliberately after SimpleMorse (Phase 12), since that game's `startTone`/`stopTone`
usage exercises the buzzer driver in a way worth having already validated.

**13.1 — Shared arc-drawing + speaker-shape helpers**
- A reusable "draw a thick curved arc" function (short connected line segments at two adjacent
  radii, since the display library has no arc primitive) and a reusable "draw a rounded-box
  speaker shape" function (box + hexagonal flare + two corner-filling circles, since there's no
  rounded-polygon primitive either). Build both as standalone functions parameterized by
  position/size/orientation now, since both the small icon, the full-screen overlay, and the
  Remote Config WiFi icon all reuse them.
- FSD section(s): §6 (intro paragraph, the shared technique).
- Definition of Done: the arc helper produces a visibly thick (not hairline) curve at both a
  small icon scale and a full-screen scale; the speaker shape helper produces no visible seam
  between the box and the flare at both scales.

**13.2 — Small persistent icon + menu integration**
- FSD section(s): §6.1, §4.2 (menu screen integration point).
- Definition of Done: icon reflects all 3 volume states correctly on the menu screen at all
  times.

**13.3 — Full-screen overlay + restore-on-release**
- Wired to the hold+turn gesture from Phase 4.2's input dispatcher. The render dispatcher itself
  must check whether the hold is still active on every redraw (not just when the gesture first
  starts), so that any other code's periodic redraw during the hold (e.g. a running timer's LED
  blink update) redraws the overlay again instead of flickering the underlying screen back into
  view.
- FSD section(s): §6.2, §3.2.
- Definition of Done: triggering the overlay from inside at least 3 different game states (not
  just the menu) correctly restores that exact prior screen on release, with no leftover visual
  artifacts from the overlay; triggering it during a game with a fast periodic redraw (e.g. a
  running Timer) does not flicker.

---

## Phase 14 — Remote Config (WiFi Web Tool)

The largest remaining phase — build the device-side API before the web page, so the page has
something real to talk to.

**14.1 — AP + captive portal + QR**
- FSD section(s): §7.1.
- Definition of Done: a phone can join the open network and land on the config page via
  auto-prompt OR by manually visiting the IP; the on-device QR code scans correctly to join the
  network.

**14.2 — Device-side API: menu/games/startup/timer**
- The `/api/config` GET+POST covering menu order, per-game enable, startup mode, default timer
  length — everything except trivia, Pass-the-Bomb-category data, and high scores.
- FSD section(s): §7.2 (sections 1–3), §7.3, §3.5 (persistence + guard rule).
- Definition of Done: a manual HTTP client (curl/Postman) can read and write every field this
  task covers, with the "at least one enabled" guard verified via a rejected-but-partial-save
  test.

**14.3 — Device-side API: trivia CSV read/write/download**
- FSD section(s): §7.2 (section 4), §7.3, §5.8 (CSV format).
- Definition of Done: GET/POST round-trip a question set correctly; the CSV download endpoint
  returns a real file download (verify the `Content-Disposition` header, not just correct body
  content — this specific detail is what makes it work in restricted mobile browsers per §7.2's
  rationale).

**14.4 — Device-side API: Pass the Bomb categories**
- FSD section(s): §7.2 (section 5), §5.5, §3.5.
- Definition of Done: same pattern as 14.2, applied to the category enable/disable data.

**14.5 — Device-side API: high score reset**
- The dedicated `POST /api/reset-highscore` endpoint (§7.3), accepting one of the three game
  keys and resetting that specific persisted score to 0. This is deliberately the *only* place
  a high score can be reset — there's no on-device equivalent by design.
- FSD section(s): §7.2 (section 6), §7.3.
- Definition of Done: resetting one game's score via this endpoint doesn't affect the other two;
  a subsequent `GET /api/config` reflects the reset immediately.

**14.6 — Web page: menu/games/startup/timer UI**
- FSD section(s): §7.2 (sections 1–3).
- Definition of Done: every control in these sections works end-to-end against the real device
  (not a mock), including the up/down reorder buttons and the toggle switches.

**14.7 — Web page: trivia editor UI**
- Including the download/upload flow.
- FSD section(s): §7.2 (section 4).
- Definition of Done: edit a question, save it, and confirm on-device Trivia gameplay reflects
  the change; download a CSV and confirm it opens correctly in a spreadsheet app.

**14.8 — Web page: Pass the Bomb categories UI**
- FSD section(s): §7.2 (section 5).
- Definition of Done: toggling a category off here and confirming it never appears in on-device
  Pass the Bomb rerolls afterward.

**14.9 — Web page: High scores UI**
- A list of the 3 games with their current best and a Reset button each, browser-confirmed
  before applying.
- FSD section(s): §7.2 (section 6).
- Definition of Done: resetting a score here updates both the on-screen number immediately and
  what's shown on the corresponding game's on-device Ready/Game Over screen next time it's
  viewed.

---

## Phase 15 — Custom Font & Visual Polish

**15.1 — Custom pixel font integration**
- Source or convert an 8×8 pixel/arcade-style bitmap font (§9) covering ASCII + é/è, in a format
  your display library can load as a secondary font alongside its default one.
- FSD section(s): §9.
- Definition of Done: the font renders correctly at 1x scale for a test string containing both
  accented characters.

**15.2 — Apply custom font to splash screens and Tetris/Star Drop title bars**
- FSD section(s): FSD §4.1, §4.3, §5.10, §5.11.
- Definition of Done: all four locations visually use the custom font; nothing else in the
  interface is affected (the default font should still be used everywhere else).

---

## Phase 16 — Full Integration Pass

**16.1 — Cross-feature regression pass**
- Manually walk every §3.2 input-model row against every single game and menu screen (not just
  the ones it was designed against) — this is where subtle cross-feature bugs hide (e.g. a
  game's context-specific encoder-click override accidentally also firing on a menu screen, or
  Snake's/SimpleMorse's encoder usage conflicting with the global volume/menu gestures).
- Definition of Done: a checklist of (screen × input gesture) combinations, each with an
  observed-vs-expected result, with zero mismatches.

**16.2 — Power management pass**
- Verify screen-sleep and deep-sleep timing and their "time-critical" exemptions across every
  applicable game state listed in §3.3 — **explicitly including leaving each game mid-round via
  the leave-confirmation dialog**, not just letting each round end naturally. This is exactly
  where a real bug shipped once (Tetris, Star Drop, Snake, and Toothbrush Timer all had this gap
  independently) — the exemption check only looks at current state, so if a game's "playing"
  state isn't explicitly reset on the leave-dialog's teardown path, sleep stays blocked forever
  afterward, even back at the menu.
- Definition of Done: each exemption condition in §3.3's list is individually tested both by
  letting the round end naturally AND by abandoning it via the leave-confirmation dialog; after
  the leave-dialog path specifically, confirm the screen *does* still sleep normally afterward
  when sitting idle at the menu.

**16.3 — Localization completeness pass**
- Switch the language to French and then Spanish and click through every single screen in the
  app, checking for un-translated (English-fallback) strings and any text that visibly
  overflows/clips instead of wrapping. Re-run the string-table position-verification check from
  3.1 one final time.
- Definition of Done: zero un-translated strings found in either language; zero visible
  clipping; the verification script reports zero position mismatches across the full table.

---

## Notes for Whoever Picks This Up

- Phases are safe to execute strictly in order; within a phase, tasks are also ordered by
  dependency unless noted otherwise.
- If a task references "reuse" of something from an earlier task (the Auto-ramp step function,
  the explosion animation, the firework animation, the arc/speaker-shape helpers, the
  word+code display function), build that shared piece as a genuinely standalone,
  independently-callable function the first time it's needed — don't copy-paste it into the
  second caller.
- Every task lists its FSD section(s) deliberately narrowly, so you don't need to hold the whole
  FSD in context to execute one task — but if anything in a task description seems to
  contradict or under-specify relative to its cited FSD section, the FSD wins; flag the
  discrepancy rather than guessing.
- Three bug classes bit this project for real during development, each now called out inline at
  the task where it's most likely to recur: (1) the string table's positional-lookup fragility
  (3.1, 16.3), (2) forgetting to reset a game's "playing" state on the leave-confirmation path,
  breaking sleep afterward (11.4/12.6/16.2), and (3) gating a cleanup action (like `stopTone()`)
  behind the same state check used to trigger the action it's cleaning up after, when that state
  can legitimately change in between (12.4). If you're implementing something structurally
  similar to any of these, it's worth deliberately checking whether the same failure mode
  applies.
