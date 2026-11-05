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
  (trivia CSVs, the config web page).
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
  and "just released" edges, exposes the current raw held/not-held state for other code to poll,
  and drives the button LED as a simple `digitalWrite` on/off (blink logic comes later, per-game).
- FSD section(s): §2.3.
- Definition of Done: a short press reliably logs exactly one "pressed" and one "released" event
  with no duplicate/missed events across ~20 manual test presses.

**1.3 — Debounced encoder reader (rotation + button)**
- Implement rotation direction detection (CLK/DT reading, §2.4) and a separately-debounced
  reader for the encoder's own push button, exposing: rotation direction events, "just pressed,"
  "just released," and continuous held-state.
- FSD section(s): §2.4.
- Definition of Done: turning the encoder one detent in each direction logs exactly one `+1` or
  `-1` event; encoder button press/release behaves like 1.2's button reader.

**1.4 — Buzzer driver**
- Set up the LEDC PWM channel on the buzzer pin; implement a `playNote(frequency, durationMs)`
  helper and a volume state (0/1/2) that maps to PWM duty per §2.5. Muted (volume 0) must skip
  the call entirely rather than playing silently, so it doesn't consume the note's duration.
- FSD section(s): §2.5.
- Definition of Done: calling `playNote()` at each of the 3 volume levels produces audibly
  different loudness on real hardware (or is at minimum wired correctly per the duty values in
  §2.5 if hardware isn't available for this task).

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
  with typed getters/setters and the documented defaults for each key. Don't wire these into any
  UI yet — just the storage layer.
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
- FSD section(s): §8.
- Definition of Done: calling `T(id)` after switching the stored language value returns the
  correct column; an id with no data for a language falls back sensibly (document your choice —
  e.g. fall back to English) rather than crashing or returning garbage.

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
  whichever condition is met first wins) and to §3.6's phantom-click note, which applies to this
  same debounce machinery.
- FSD section(s): §3.2, §3.6.
- Definition of Done: manually test every row of the §3.2 table in isolation (a short click, a
  long-press with no rotation, a hold+turn, a hold+turn followed by continued turning after the
  long-press fires) and confirm each produces exactly the documented behavior with no
  cross-triggering.

**4.3 — Main menu list + scrolling**
- Implement the 3-row scrollable menu list, default order (§4.2), icon+label rendering,
  highlight-inverts-the-row rendering, and confirm-to-select. Games themselves don't exist yet —
  confirming an entry can just log/stub "entered game N" for now.
- FSD section(s): §4.2.
- Definition of Done: all default-order entries are reachable by scrolling in both directions,
  wrapping behavior at the list ends matches your chosen convention (document it), and the
  cursor always starts on the first entry after a fresh boot.

**4.4 — Settings submenu (Language + Timer Step + Config Mode entry)**
- Build the 3-row Settings list and the Language/Timer Step picker screens, wired to the
  persistence layer (2.1) and string table (3.1). The Config Mode row can stub-log "would start
  Config Mode here" until Phase 12.
- FSD section(s): §4.5.
- Definition of Done: changing language and confirming immediately re-renders the settings list
  itself in the new language (proves the live-switch works, not just storage).

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
- The 1-second icon+name splash shown when entering any game from the menu (§4.3 of the FSD —
  note this is FSD §4.3, distinct from this plan's own numbering).
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
  declares the correct winner.

**6.3 — Toothbrush Timer game**
- FSD section(s): §5.9. Reuses 6.1's "firework" celebration animation for completion (build that
  as reusable too, or generalize Reflex's if it wasn't already general-purpose).
- Definition of Done: 1–5 minute selection, 4 equal quadrant segments with a beep on each
  transition, pause/resume, completion animation plays once.

---

## Phase 7 — Pass the Bomb

Broken out as its own phase since it has real content (20 categories) and a novel mechanic
(the reroll animation) not shared with anything built so far.

**7.1 — Category data + enable/disable storage**
- Define the 20 categories from §5.5 with their EN/FR/ES translations in the string table
  (Phase 3), and a persisted enabled/disabled flag array (extend Phase 2's persistence layer)
  with the "at least one enabled" guard rule from §3.5.
- FSD section(s): §5.5 (category list), §3.5 (persistence + guard rule).
- Definition of Done: all 20 categories are individually toggle-able in isolation (a temporary
  debug UI or direct function calls are fine for this task — the real web UI comes in Phase 12);
  attempting to disable the last remaining one is rejected.

**7.2 — Ready screen + reroll**
- The category+letter combo, the left-2/3/right-1/3 layout, and the slot-machine reroll
  animation triggered by encoder rotation while ready.
- FSD section(s): §5.5.
- Definition of Done: reroll only ever lands on enabled categories; the animation visibly
  decelerates rather than looking like a single instant swap.

**7.3 — Armed/explode logic**
- The hidden 10–60s timer, the deliberately-blank "no hint" behavior, the fixed-rate LED blink,
  and the explosion → "BOOM!" flow reusing 6.1's shared animation.
- FSD section(s): §5.5.
- Definition of Done: across several armed rounds, the LED blink rate is visibly constant
  regardless of how close the hidden timer is to firing (this is the thing most likely to get
  implemented wrong by "helpfully" adding a speed-up — explicitly verify it does NOT speed up).

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
- Selection screens, then the once-per-round shuffle of that file's questions into a fixed
  session order.
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

**9.3 — Controls (move/rotate/soft-drop) + gravity/speed ramp**
- FSD section(s): §5.10.
- Definition of Done: encoder moves left/right respecting collision, encoder click rotates
  (silently), holding the arcade button visibly speeds up the fall, normal fall speed increases
  every 10 lines per the documented floor.

**9.4 — Line clear + scoring + game over**
- The flash-before-remove animation, the scoring table, and the topping-out game-over sequence.
- FSD section(s): §5.10.
- Definition of Done: single and multi-line clears score per the documented table × level
  multiplier; the flash is visibly a toggle, not instant; game over plays the full described
  sequence.

**9.5 — Ready/Game Over screens + inverted title bar**
- Including the word-boundary-safe two-line wrap for translations too wide for the rotated
  64px width.
- FSD section(s): §5.10, §8 (wrap rule).
- Definition of Done: French/Spanish "Game Over" (both notably longer than English) wrap cleanly
  onto two lines rather than overflowing or breaking mid-word.

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
- Definition of Done: matches 9.5's acceptance criteria, applied to Star Drop's own screens.

---

## Phase 11 — Volume System

Deliberately built after the games exist, since the full-screen overlay needs to correctly
suspend and restore whatever screen was active underneath it — easier to test with real games in
place.

**11.1 — Small persistent icon + menu integration**
- FSD section(s): §6.1, §4.2 (menu screen integration point).
- Definition of Done: icon reflects all 3 volume states correctly on the menu screen at all
  times.

**11.2 — Full-screen overlay + restore-on-release**
- Wired to the hold+turn gesture from Phase 4.2's input dispatcher.
- FSD section(s): §6.2, §3.2.
- Definition of Done: triggering the overlay from inside at least 3 different game states (not
  just the menu) correctly restores that exact prior screen on release, with no leftover visual
  artifacts from the overlay.

---

## Phase 12 — Config Mode / Web Tool

The largest remaining phase — build the device-side API before the web page, so the page has
something real to talk to.

**12.1 — AP + captive portal + QR**
- FSD section(s): §7.1.
- Definition of Done: a phone can join the open network and land on the config page via
  auto-prompt OR by manually visiting the IP; the on-device QR code scans correctly to join the
  network.

**12.2 — Device-side API: menu/games/startup/timer**
- The `/api/config` GET+POST covering menu order, per-game enable, startup mode, default timer
  length — everything except trivia and Pass-the-Bomb-category data.
- FSD section(s): §7.2 (sections 1–3), §7.3, §3.5 (persistence + guard rule).
- Definition of Done: a manual HTTP client (curl/Postman) can read and write every field this
  task covers, with the "at least one enabled" guard verified via a rejected-but-partial-save
  test.

**12.3 — Device-side API: trivia CSV read/write/download**
- FSD section(s): §7.2 (section 4), §7.3, §5.8 (CSV format).
- Definition of Done: GET/POST round-trip a question set correctly; the CSV download endpoint
  returns a real file download (verify the `Content-Disposition` header, not just correct body
  content — this specific detail is what makes it work in restricted mobile browsers per §7.2's
  rationale).

**12.4 — Device-side API: Pass the Bomb categories**
- FSD section(s): §7.2 (section 5), §5.5, §3.5.
- Definition of Done: same pattern as 12.2, applied to the category enable/disable data.

**12.5 — Web page: menu/games/startup/timer UI**
- FSD section(s): §7.2 (sections 1–3).
- Definition of Done: every control in these sections works end-to-end against the real device
  (not a mock), including the up/down reorder buttons and the toggle switches.

**12.6 — Web page: trivia editor UI**
- Including the download/upload flow.
- FSD section(s): §7.2 (section 4).
- Definition of Done: edit a question, save it, and confirm on-device Trivia gameplay reflects
  the change; download a CSV and confirm it opens correctly in a spreadsheet app.

**12.7 — Web page: Pass the Bomb categories UI**
- FSD section(s): §7.2 (section 5).
- Definition of Done: toggling a category off here and confirming it never appears in on-device
  Pass the Bomb rerolls afterward.

---

## Phase 13 — Custom Font & Visual Polish

**13.1 — Custom pixel font integration**
- Source or convert an 8×8 pixel/arcade-style bitmap font (§9) covering ASCII + é/è, in a format
  your display library can load as a secondary font alongside its default one.
- FSD section(s): §9.
- Definition of Done: the font renders correctly at 1x scale for a test string containing both
  accented characters.

**13.2 — Apply custom font to splash screens and Tetris/Star Drop title bars**
- FSD section(s): FSD §4.1, §4.3, §5.10, §5.11.
- Definition of Done: all four locations visually use the custom font; nothing else in the
  interface is affected (the default font should still be used everywhere else).

---

## Phase 14 — Full Integration Pass

**14.1 — Cross-feature regression pass**
- Manually walk every §3.2 input-model row against every single game and menu screen (not just
  the ones it was designed against) — this is where subtle cross-feature bugs hide (e.g. a
  game's context-specific encoder-click override accidentally also firing on a menu screen).
- Definition of Done: a checklist of (screen × input gesture) combinations, each with an
  observed-vs-expected result, with zero mismatches.

**14.2 — Power management pass**
- Verify screen-sleep and deep-sleep timing and their "time-critical" exemptions across every
  applicable game state listed in §3.3.
- Definition of Done: each exemption condition in §3.3's list is individually tested (e.g.
  confirm the screen does NOT sleep mid-Tetris-game, but DOES sleep if Tetris is sitting idle on
  its Ready screen).

**14.3 — Localization completeness pass**
- Switch the language to French and then Spanish and click through every single screen in the
  app, checking for un-translated (English-fallback) strings and any text that visibly
  overflows/clips instead of wrapping.
- Definition of Done: zero un-translated strings found in either language; zero visible
  clipping.

---

## Notes for Whoever Picks This Up

- Phases are safe to execute strictly in order; within a phase, tasks are also ordered by
  dependency unless noted otherwise.
- If a task references "reuse" of something from an earlier task (the Auto-ramp step function,
  the explosion animation, the firework animation), build that shared piece as a genuinely
  standalone, independently-callable function the first time it's needed — don't copy-paste it
  into the second caller.
- Every task lists its FSD section(s) deliberately narrowly, so you don't need to hold the whole
  FSD in context to execute one task — but if anything in a task description seems to
  contradict or under-specify relative to its cited FSD section, the FSD wins; flag the
  discrepancy rather than guessing.
