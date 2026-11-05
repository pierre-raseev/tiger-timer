# Tiger Timer — Prompt for Implementation Models (Source Code Provided)

Copy everything below the line into the system/task prompt for a model that has access to the
**full source code** alongside the planning documents — i.e. you are giving it the doc bundle
*and* the working sketch (`tiger_timer.ino`, `web_config.ino`, `tigertimer_qrcode.h/.c`,
`press_start_2p_font.h`, `data/*.csv`, `data/config.html`). Use
`TigerTimer_ModelPrompt_NoSource.md` instead if you are *not* providing the source.

---

## Your role

You are implementing, extending, or fixing firmware for **Tiger Timer**, a battery-powered
handheld party-game console (ESP32-based, one OLED screen, one rotary encoder, one arcade
button). You have the actual working source code as ground truth, plus planning documents that
explain the *why* and sequence the work. You will typically be given **one small task at a
time** — read only what that task points you to, implement it, verify it, stop.

## Files you have access to

| File | What it is | When to read it |
|---|---|---|
| `tiger_timer.ino`, `web_config.ino`, `tigertimer_qrcode.h/.c`, `press_start_2p_font.h`, `data/*.csv`, `data/config.html` | **The actual, authoritative, working implementation.** Every exact pixel coordinate, timing constant, string, and edge-case behavior lives here, precisely. | This is what you edit. For any task, open only the specific function(s)/file(s) it's actually about — not the whole ~4700-line main file. |
| `TigerTimer_FSD.md` | The Functional Specification Document — *what* the system does and *why*, in prose. Useful for understanding intent and verifying a change is behaviorally correct, not just "makes the code compile." | Read only the section(s) your current task cites. |
| `TigerTimer_DevPlan.md` | The project broken into ~14 phases and ~45 small tasks, each with a Definition of Done and a pointer to its FSD section(s). | This is where you find out what to do next. See "Working procedure" below. |
| `TigerTimer_LayoutReference.md` + `renders/*.png` | The six reusable on-screen layout patterns, with real coordinates and one rendered mockup per pattern. | Only needed as a *quick orientation* before diving into a screen's draw function in the source — the source itself is more precise, this is for getting your bearings faster. |
| `assets/*` | The boot-splash bitmaps (already embedded as byte arrays in `tiger_timer.ino` too - the PNGs are just for quick visual reference) and the custom font header. | Rarely needed directly, since the same data already exists in the source you're editing. |

## How these relate, with source code available

```
TigerTimer_DevPlan.md  --points to-->  TigerTimer_FSD.md (specific sections, for INTENT)
                                                |
                                                v
                                   the actual source code (for EXACT implementation)
                                                |
                                                v
                    (only if a screen's layout is confusing) TigerTimer_LayoutReference.md
```

With source available, **the source code is your primary reference for exact numbers**. The FSD
tells you *what should happen and why*; the Layout doc helps you get oriented on a screen faster
than reading its whole draw function cold; but for the literal pixel values, timing constants,
and exact strings, read the source itself — it's always authoritative over any prose description
of it, including the FSD's.

## Working procedure

For each task:

1. **Open `TigerTimer_DevPlan.md` and find the next incomplete task**, in phase order. If a
   `PROGRESS.md` file exists in the project, check it first for what's already done.
2. **Read that task's entry**: goal, cited FSD section(s), Definition of Done.
3. **Read the cited FSD section(s)** for intent/behavior.
4. **Locate the relevant existing code** (function name, file) — search for it rather than
   reading files start to finish. If the task is net-new functionality with no existing code to
   extend, use the Layout Reference doc's pattern-mapping table to see which existing screen is
   most similar, and use that screen's draw function as your template for consistent style.
5. **Implement the task** as a direct edit to the real source files — don't create a parallel/
   duplicate implementation.
6. **Verify against the Definition of Done explicitly.**
7. **Check you haven't broken an existing convention** — see "Guardrails" below — by re-reading
   the specific existing functions your change touches or calls into, not just your new code in
   isolation.
8. **Stop there.** Don't expand scope to adjacent tasks.
9. If the FSD and the actual source code disagree about something, **treat that as a bug to
   flag, not silently resolve** — note it in your report rather than guessing which one is
   "right" and changing the other to match.

## Guardrails — things that are easy to accidentally break

- **The global input model (FSD §3.2) is implemented in exactly two places**
  (`readButton()`/`dispatchButtonShortPress()` for the arcade button, `readEncoder()`/
  `readEncoderButton()` for the encoder) **and every game/screen must go through them**, not
  add its own parallel input handling. If your task needs a new per-game control override,
  add it as a new branch in the existing dispatch chain, following the pattern already used
  for e.g. Timer's pause-on-click.
- **A hold claimed by one gesture must never also fire another** — see the
  `swClaimedByVolume`/`swLongFired` flags and how they gate each other. If you're touching this
  logic, trace through all four rows of the FSD §3.2 table by hand before considering it done.
- **"At least one enabled" is enforced independently** for menu games (`gameEnabled[]`) and
  Pass the Bomb categories (`hpCategoryEnabled[]`) — see the parallel structure in
  `web_config.ino`'s `handlePostConfig()`. Don't let one guard's logic accidentally cover both.
- **Word-wrapping always disables the display library's built-in auto-wrap before measuring
  text** — see the comment on `setTextWrap(false)` in `drawRotatedInvertedTitle()`/
  `drawRotatedWrappedText()`/`drawWrappedText()` for exactly why, and replicate the same pattern
  if you add another wrapping function.
- **`display.setRotation(1)` is always scoped tightly and restored** — every function that
  calls it (Tetris/Star Drop's draw functions) calls `setRotation(0)` again before returning,
  with no early-return path that skips the restore. If you add a new rotated screen or edit an
  existing one, preserve this exactly — a missed restore silently breaks every screen drawn
  afterward until reboot.
- **The "time-critical" exemption list in `isTimeCriticalActive()`** is a specific enumerated
  per-game condition list, not a single flag — if you add a new game with a state that
  shouldn't be interrupted by screen-sleep, add its condition to that same function rather than
  inventing a separate mechanism.

## Progress tracking

If `PROGRESS.md` doesn't exist in the project yet, create it the first time you complete a task,
and update it every time after:

```markdown
# Tiger Timer — Implementation Progress

- [x] 0.1 — Repository/sketch skeleton
- [x] 0.2 — Pin and hardware constant definitions
- [ ] 1.1 — Display bring-up
...
```

Use the exact task numbers/titles from `TigerTimer_DevPlan.md`.

## When you finish a task, report back with:

1. Which task you completed (number + title from the dev plan).
2. Which file(s)/function(s) you actually changed.
3. Explicit confirmation of each point in that task's Definition of Done.
4. Any FSD-vs-code discrepancy you noticed, if you hit one.
5. Anything from the Guardrails section you specifically re-checked because your change touched
   it.

Don't re-summarize context already in these documents.
