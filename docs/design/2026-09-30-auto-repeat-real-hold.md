# 2026-09-30 — X3/X4 (C3) double button input: auto-repeat must not count render stalls as hold time

## Symptom
Every physical button registers as two navigation events on the Xteink X3/X4
(C3) release binary; the X4 Pro (async input) is unaffected. Confirm/Back
appear exempt. (2026-09-30 bug report.)

## Root cause (mechanical, C3-specific)
The X3/X4 `gh_release` build has no PSRAM, so `BOARD_HAS_PSRAM` is undefined
and `HalGPIO::begin()` never calls `inputMgr.beginAsync()`
(`lib/hal/HalGPIO.cpp:152-159`). There is no async poll task: `update()` runs
on the app's `loop()` task, and the C3 is single-core, so a full e-ink refresh
(1.3-2 s) BLOCKS the loop.

One physical press therefore produces two navigation steps:

1. The press edge commits through the debounce and is published to the app
   latch (`InputManager::applyStateChange`, stamped `buttonPressStart`).
2. `MappedInputManager::update()` snapshots it once per tick. The activity's
   `ButtonNavigator::onNextPress` fires -> step 1 -> `requestUpdate()` ->
   the ActivityManager renders -> `displayBuffer()` blocks the loop for 1-2 s.
3. The user is STILL HOLDING — they must be, the screen has shown nothing yet.
4. The first loop tick after the render runs `ButtonNavigator::onContinuous`
   (`src/util/ButtonNavigator.cpp`). No edge this tick, so it does not
   early-return, and `shouldNavigateContinuously()` passes both gates:
   `getHeldTime() > 500` is TRUE because held time is wall-clock since
   `buttonPressStart` (>= 1.3 s ago), and `millis() - lastContinuousNavTime >
   500` is TRUE because the edge tick had just reset `lastContinuousNavTime`
   to 0. -> step 2 -> a second full render.
5. The user sees step 1 appear and be immediately replaced by step 2.

The transport itself is not at fault: `InputManager` emits exactly one edge per
press; the doubling is the app-layer auto-repeat gate reading inflated
wall-clock held time across a render stall.

Why Confirm/Back are exempt: they are dispatched from `wasReleased()` /
`handleButtons()` paths with no `onContinuous` auto-repeat, and the Nav
composites never include Confirm or Back (`src/MappedInputManager.cpp`).

Why the X4 Pro is immune: its 10 ms async poll task samples through the render
and drains the release edge during it, so `isPressed()` is false on resume and
held time tracks the real hold.

## Fix (app layer, no SDK behavior change)
Owner rule implemented: buttons must never feel dead during rendering; the
acceptable pattern is to not count render-stall time as hold time. Deliberate
hold-repeat on boards that already behave correctly must not slow down.

1. `MappedInputManager` (`src/MappedInputManager.h/.cpp`) tracks render stalls:
   `lastFrameAtMs` / `stallAccumMs`, sampled at the top of every `update()`.
   Only an inter-tick gap in excess of `kFrameStallThresholdMs` (100 ms, far
   above a normal 10-50 ms tick) accrues, so ordinary jitter is never charged.
   The accumulator **saturates** (it is never capped): the hold time it is
   subtracted from is uncapped wall clock, so capping the discount would leave
   the remainder as hold time and re-arm the repeat for any stall longer than
   the cap — a multi-second chapter build on the C3 (qodo #1, copilot).
   The discount is cleared only when a press edge **starts** a contact
   (`repeathold::startsNewContact`: press edge AND no button held on the
   previous dispatch). The SDK's held clock is aggregate — it runs from the
   first button down — so a second button pressed while the navigation button
   is still held, or an edge parked across a blocking transfer, must not wipe
   the discount belonging to that still-held contact (qodo #2, kody).
2. `getRepeatHeldTime()` = `getHeldTime()` minus the accrued stalls
   (saturating at 0). It shares `getHeldTime()`'s Home-action and
   touch-override preconditions. `getHeldTime()` itself is UNCHANGED, so
   long-press detection (`wasLongPressed`) and every other consumer keep raw
   wall-clock semantics.
3. `ButtonNavigator::shouldNavigateContinuously()` reads
   `getRepeatHeldTime()` instead of `getHeldTime()`. The edge tick now sets
   `lastContinuousNavTime = millis()` (was 0) so the interval floor starts at
   the edge rather than at boot and cannot pass trivially on the first
   post-stall tick. `onRelease`'s "no repeat happened yet" check moves from
   `lastContinuousNavTime == 0` to a dedicated `continuousNavActive` flag,
   which keeps that semantic now that the floor is non-zero.
4. Gating, so X4 Pro / touch / async boards are byte-identical:
   `MappedInputManager::kRepeatHoldExcludesStalls()` is a single shared inline
   (`false` when `BOARD_HAS_PSRAM`). With it off, `getRepeatHeldTime()` returns
   `getHeldTime()` and the edge tick keeps `lastContinuousNavTime = 0`.

## Test plan
`src/util/RepeatHoldDiscount.h` holds the discount policy as pure constexpr
helpers (`stallFor`, `addSaturated`, `discount`, `startsNewContact`) plus
`StallDiscountWindow`, the per-dispatch state machine that
`MappedInputManager::update()` actually drives (`sampleFrame` at the top of the
dispatch, `deliverFrame` at the end, `heldMs` for the gated repeat check). The
split exists because `update()` is not host-compilable: with the wiring inline
in the class, only the pure predicate was covered and the real frame sequence
was untested (kody round 2). `MappedInputManager` owns one window and calls it.
The host stub `MappedInputManager` calls the same `repeathold::discount`, so
the navigator tests cannot pass against a weakened discount.

- `test/button_press_navigation` (host gtest, 19 tests):
  - `RenderStallWhileStillHeldDoesNotRepeatThePress` — REGRESSION: press edge,
    2 s stall, still held -> `pages == 0`. Fails on the pre-fix navigator
    (verified by rebuilding against `develop`'s ButtonNavigator).
  - `VeryLongStallStillCoversTheWholeHold` — a 30 s stall covers the whole
    30 s hold (qodo #1 / copilot).
  - `DeliberateHoldWithoutStallStillRepeats` — a genuine 1.5 s hold with no
    stall still repeats once.
  - `ReleaseAfterARepeatDoesNotStepAgain` — a fired repeat suppresses the
    release step on every board.
  - `RepeatHoldDiscountTest.*` — normal ticks accrue nothing; only the excess
    over the threshold is a stall; accumulation saturates instead of capping;
    discount saturates at zero; only an opening press edge starts a contact.
  - `StallDiscountWindowTest.*` — the frame-by-frame wiring the C3 runs: 12
    ordinary ticks then a 2 s render stall (hold collapses to the real 120 ms
    of ticks plus the gap's own 100 ms threshold, far under the 500 ms repeat
    gate), a second press mid-hold keeping the discount, a new contact after a
    full release starting clean, and a 30 s build stall covering a 30 s hold.
- All three defect shapes are mutation-checked: the 10 s cap, the unconditional
  press-edge reset in `MappedInputManager::update()`, and an unconditional reset
  inside `StallDiscountWindow::deliverFrame` each fail the suite.
- Full host `ctest` suite; `pio run -e default` (C3) and `pio run -e x4pro`
  (S3); `pio check` (cppcheck); `clang-format` via the wrapper.

Device confirmation remains outstanding (owner has no X3/X4 unit on hand);
this ships on the strength of the code-level mechanism and the host regression
test.
