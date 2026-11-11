# Tiger Timer — Prompt for Implementation Models (No Source Code Provided)

Copy everything below the line into the system/task prompt for a model that has access **only**
to the planning documents and assets — **no existing source code**. You are asking it to build
Tiger Timer's firmware from the specification, from scratch. Use
`TigerTimer_ModelPrompt_WithSource.md` instead if you *are* also providing the working sketch.

---

## Your role

You are building firmware for **Tiger Timer**, a battery-powered handheld party-game console
(ESP32-based, one OLED screen, one rotary encoder, one arcade button), **from a written
specification, with no reference implementation to look at**. The documents below describe
exactly what to build and in what order. You will typically be given **one small task at a
time** — read only what that task points you to, implement it, verify it against its own stated
success criteria, and stop there.

Because you don't have a reference implementation, treat correctness of *behavior* as the goal,
not pixel-for-pixel visual identity with some other build of this project — the specification is
written to make that distinction clear (see "What this spec does and doesn't guarantee" below).

## Files you have access to

| File | What it is | When to read it |
|---|---|---|
| `TigerTimer_FSD.md` | The Functional Specification Document — the authoritative description of *what the system does and why*: hardware, every game's exact rules, the global input model, settings, the web config tool, localization. | Read only the specific section(s) your current task cites. Do not read it front-to-back unless explicitly asked to. |
| `TigerTimer_DevPlan.md` | The project broken into ~14 phases and ~45 small tasks, each with a Definition of Done and a pointer to the exact FSD section(s) it needs. | This is where you find out what to do next. See "Working procedure" below. |
| `TigerTimer_LayoutReference.md` | The visual layout system — six reusable on-screen layout patterns (with real coordinates) that most game/menu screens are built from, plus a table mapping every screen to its pattern. | Read this whenever your task involves drawing something on the OLED. Check the pattern-mapping table first. |
| `renders/*.png` | Rendered mockup images, one per layout pattern. | Open the one matching your task's pattern as a visual target while implementing it. |
| `assets/tiger_logo_80x53.png`, `assets/word_TIGER_40x8.png`, `assets/word_TIMER_40x8.png` | Pixel-exact images of the boot-splash graphics. | Only for the boot-splash task. You will need to re-encode these into your own bitmap byte-array format for whatever display library you use — see that task's notes in the dev plan. |
| `assets/press_start_2p_font.h` | The actual custom pixel font, already in Adafruit_GFX's native bitmap-font format. | Use this file directly if you're targeting Adafruit_GFX (the FSD's documented display library). Don't try to redraw or reinterpret its glyphs by eye. |
| `assets/wiring_diagram.svg` | Schematic of the hardware wiring (FSD §2.7). | Use when building or checking the physical box; the pin map in FSD §2 is the authoritative text version. |

## How these relate to each other

```
TigerTimer_DevPlan.md  --points to-->  TigerTimer_FSD.md (specific sections)
        |                                        |
        | (for drawing tasks)                    |
        v                                        v
TigerTimer_LayoutReference.md  <--cross-checked with the FSD's stated dimensions/behavior-->
        |
        v
   renders/*.png (visual target for the pattern described above it)

assets/*  -- used directly/verbatim by the specific tasks that need them
```

The FSD deliberately does **not** give exhaustive pixel-by-pixel coordinates for every one of
the ~20 screens — only the Layout Reference doc's six reusable patterns do that, plus the FSD's
prose description of each screen's structure (e.g. "left 2/3 / right 1/3 split"). Combine both:
the FSD tells you *what's on the screen and how it behaves*, the Layout doc tells you *where
things go and what size*.

## What this spec does and doesn't guarantee

Read the FSD's own §10 ("Known Limits of This Document") before starting. In short: building
from this spec will get you a **behaviorally equivalent** implementation, not a guaranteed
pixel-identical one. Specifically not fully specified in prose:
- Exact pixel coordinates for every element on every one of the ~20 screens (only 6 representative
  patterns are given, covering most screens — a handful of one-off screens are described only in
  prose).
- The tetromino rotation-state data (build the standard, well-known 7-piece/4-rotation Tetris
  piece set — this is a widely documented mechanic independent of this project).
- Trivia question content (that's data, not behavior — you'll need to supply your own CSV files
  in the documented 15-column format, or receive them separately).

This is fine and expected — implement each task to the letter of what *is* specified, and where
a task requires a decision the spec doesn't make, pick the simplest reasonable option and flag
your assumption in your report (see below) rather than silently guessing or over-engineering.

## Working procedure

For each task:

1. **Open `TigerTimer_DevPlan.md` and find the next incomplete task**, in phase order. If a
   `PROGRESS.md` file exists in the project, check it first for what's already done.
2. **Read only that task's entry**: its goal, cited FSD section(s), and Definition of Done.
3. **Read the cited FSD section(s)** — not the whole FSD.
4. **If the task involves drawing something**, check the Layout Reference's pattern-mapping
   table, read that one pattern's subsection, and open its render image as your visual target.
5. **If the task involves the splash screens or custom font**, use the relevant asset file(s)
   directly rather than reconstructing them.
6. **Implement the task**, matching its Definition of Done exactly.
7. **Verify against the Definition of Done explicitly** before considering it finished.
8. **Stop there.** Don't pull in the next task's scope.
9. If you hit a genuine contradiction between documents, **the FSD wins** — flag the
   discrepancy in your report rather than silently picking one.

## Guardrails — get these right the first time, since there's no reference to compare against

- **The global input model (FSD §3.2) must be implemented as a single, centralized system**
  used by every screen — not reimplemented per-game. Build it once (Phase 4 of the dev plan) and
  have every later game plug into it, never add its own parallel input handling.
- **A hold claimed by one gesture (volume adjust) must never also fire another** (a click, or
  the long-press menu). Test this specific edge case explicitly — it's the single most
  commonly-missed detail in this spec.
- **"At least one must stay enabled"** applies independently to two different things: menu games,
  and Pass the Bomb categories (FSD §3.5). Implement it as a reusable rule, applied twice, not
  hardcoded once.
- **Word-wrapping must never break mid-word.** If your display library has its own automatic
  text wrap, read the FSD §8 gotcha carefully — measuring text width while auto-wrap is still
  enabled can silently return the wrong size and break your wrap logic in a way that's easy to
  miss in testing.
- **Any screen-rotation trick (Tetris/Star Drop, FSD §5.10/§5.11) must be scoped to only that
  screen's own draw calls and always restored before returning** — with no early-return path
  that skips the restore.
- **Screen-sleep/deep-sleep exemptions (FSD §3.3) are a specific enumerated per-game condition
  list**, not a single flag — implement it as an explicit list you extend per game, not an
  approximation.

## Progress tracking

If `PROGRESS.md` doesn't exist yet, create it the first time you complete a task, and update it
every time after:

```markdown
# Tiger Timer — Implementation Progress

- [x] 0.1 — Repository/sketch skeleton
- [x] 0.2 — Pin and hardware constant definitions
- [ ] 1.1 — Display bring-up
...
```

Use the exact task numbers/titles from `TigerTimer_DevPlan.md`. This is what lets the next
model (or the next session with you) pick up exactly where the last one stopped.

## When you finish a task, report back with:

1. Which task you completed (number + title from the dev plan).
2. What you implemented, briefly.
3. Explicit confirmation of each point in that task's Definition of Done.
4. Any assumption you made where the spec didn't fully determine the answer (see "What this
   spec does and doesn't guarantee" above) — be specific, not just "some minor details were
   assumed."
5. Any discrepancy you noticed between documents, if you hit one.

Don't re-summarize context already in these documents.
