# Avg Session: Kindle-Style Trimmed-Mean Window

- **Date:** 2026-09-10
- **Status:** Approved for implementation (user-approved parameters)
- **Scope:** `src/activities/reader/` reading-stats subsystem
- **Follows:** `2026-09-01-kindle-wpm-word-count-plumbing.md` (the WPM window
  this design mirrors)

## 1. Problem

The **Avg Session** cell on both stats screens is computed as
`totalReadingSeconds / sessionCount` — a plain arithmetic mean over the
book's (or the user's) entire history:

- `BookStatsView.cpp` (per-book): `stats.sessionCount > 0 ?
  stats.totalReadingSeconds / stats.sessionCount : 0`
- `BookStatsView.cpp` (global): `stats.totalSessions > 0 ?
  stats.totalReadingSeconds / stats.totalSessions : 0`

One anomalous session — a book left open overnight, a marathon weekend —
permanently skews the average until enough ordinary sessions dilute it.
Kindle instead keeps a **window of recent sessions**, drops the highest and
lowest outliers, and averages the rest. The reading-speed metric in this
subsystem already uses exactly this shape (`WpmWindow`, trim-2/trim-2 over a
15-sample window). This design applies the same algorithm to session
durations.

Non-goal: changing the **Sessions** count, **Reading Time** total, or any
other existing cell. The legacy arithmetic mean remains as a fallback (§6).

## 2. Algorithm

### 2.1 The window

A `SessionWindow` struct in `src/activities/reader/ReadingStatsUtils.{h,cpp}`,
structurally identical to `WpmWindow`:

| Constant | Value | Meaning |
|---|---|---|
| `SESSION_WINDOW_SIZE` | 10 | rolling window of session durations |
| `SESSION_TRIM_COUNT` | 2 | drop the 2 longest + 2 shortest before averaging |
| `SESSION_MIN_SECONDS` | 30 | a shorter reading session is not recorded |
| `SESSION_MIN_PAGE_TURNS` | 2 | a session with fewer completed forward page turns is "book left open" — not recorded |
| `SESSION_MIN_DISPLAY_SAMPLES` | 4 | sessions recorded before the Avg Session cell shows the window's result |

```
record(seconds):   # caller gates on >= SESSION_MIN_SECONDS AND >= SESSION_MIN_PAGE_TURNS
    if seconds < SESSION_MIN_SECONDS: return        # glance/abort sessions
    if seconds > UINT16_MAX: seconds = UINT16_MAX   # overflow clamp (~18.2 h)
    samples[pos] = seconds
    pos = (pos + 1) % SESSION_WINDOW_SIZE
    if count < SESSION_WINDOW_SIZE: count += 1
    avg = trimmedMean()
```

`trimmedMean()` — same shape as `WpmWindow::trimmedMean()`: `count == 0`
returns 0; otherwise insertion-sort the
live samples (10 × `uint16_t` = 20 B stack scratch), drop
`SESSION_TRIM_COUNT` from each end, average the middle
`count - 2 * SESSION_TRIM_COUNT`. When `count <= 2 * SESSION_TRIM_COUNT` (≤ 4
samples) the window is too small to trim and the plain arithmetic mean is
returned. **Window size rationale (user decisions, 2026-09-10):** sessions are
far rarer than page turns — an 18-session window could take weeks to fill,
leaving the trimmed mean as a long-horizon partial mean. 10 samples fills in
roughly two weeks of regular reading while keeping the trim meaningful (mean
of the middle 6 when full).

`normalize()` — same invariant as `WpmWindow::normalize()`, post-load: a
partial window (`count < SESSION_WINDOW_SIZE`) must have `pos == count`; a
full window with an out-of-range `pos` resets to 0; `avg` is always
recomputed from the window, never trusted from disk.

Unit: **seconds** in a `uint16_t` (user decision; final window size 10 after
the 18 → 8 → 10 iteration). The display cell renders
via `formatDuration()` at minute granularity, so the 65 535 s (~18.2 h) clamp
is invisible; it exists purely so the sample math cannot overflow. There is
**no plausibility cap** and **no floor** beyond the 30 s entry gate (user
decision): a 3-hour session is legitimate data and the trim-2 step handles
true outliers.

### 2.2 Where samples come from

`EpubReaderActivity::onExit()` already computes the session's elapsed reading
seconds (`sessionReadingSeconds`). Today:

```cpp
if (elapsedSecs >= 60) { stats.sessionCount++; globalStats.totalSessions++; }
if (elapsedSecs >= 10) { ... totals, buckets, history ... }
```

Added alongside (after the existing blocks):

```cpp
if (elapsedSecs >= SESSION_MIN_SECONDS) {
  stats.recordSession(elapsedSecs);
  globalStats.recordGlobalSession(elapsedSecs);
}
```

**Decision D1 — the window has its own gates.** The `sessionCount`
/"Sessions" cells keep their existing ≥60 s gate so that persisted counter
keeps its historical semantics. The window is a separate metric with its own
gates: a **lower** time threshold (30 s) **plus an engagement gate**
(`SESSION_MIN_PAGE_TURNS = 2` completed forward page turns, user decision
2026-09-10 — "1 vs 2" was left to the implementer; 2 wins because a single
page can still be a glance-and-go, while two turns show sustained reading).
A book merely left open turns no pages and is never recorded, no matter how
long it sat open. A 45-second session with 2+ turns counts toward Avg
Session's window but not toward the Sessions cell; each persisted counter
keeps its historical semantics, and exactly one metric changes.

**Engineering paradigm (user directive, 2026-09-10):** this subsystem — and
crosspoint work generally — follows **DRY, Solid, KISS**: one counter shared
by the existing forward-turn recording path rather than a parallel callback;
each gate a plain constant comparison; no abstraction beyond the problem.

## 3. Persistence

Both records grow by the same trailing block — the session window in the
same wire shape the WPM window uses (`avg` u16, `count` u16, samples u16 ×N,
`pos` u8 = 5 + 2N bytes):

```
SessionWindow wire (N = SESSION_WINDOW_SIZE = 10) = 5 + 20 = 25 bytes
```

### 3.1 Per-book: v6 → **v7** (109 B → **134 B**)

```
stats_v7.bin (134 bytes) = v6 fields unchanged (bytes 0-108) +
  [109-110] sessionWindow.avg      uint16_t LE, trimmed mean seconds (0 = none)
  [111-112] sessionWindow.count    uint16_t LE, samples in window (0-10)
  [113-132] sessionWindow.samples  uint16_t LE each
  [133]     sessionWindow.pos      uint8_t
```

- `decodeV7` calls the existing `readV5Fields` helper, then `readWpmWindow`
  (bytes 73-108, already version-agnostic), then reads + normalizes the
  session window. **It must not route through `decodeV6`/`decodeV5`** — the
  version-check trap documented in the skill (a v7 record would be rejected
  by their `data[0] != expected` checks).
- `decodeV6` stays byte-for-byte as-is (109 B records load with an empty
  session window) — it is the immediately-previous-version migration path.
- Migration: first `save()` of a loaded v6 record writes `stats_v7.bin` and
  deletes `stats_v6.bin` **only after** the v7 write returns the full size
  (hard rule #2: never destroy the legacy record on a short write).
- `openCandidateNames()` becomes `{v7, v6, v5}`: the v5 record remains a
  recognized load candidate (empty windows), and its first save jumps
  straight to v7 — the writer only ever emits the current version, so the
  old "v5 → v6, then v6 → v7" two-build chain collapses into one hop. The
  migration delete (and `BookReadingStats::remove`) covers **both**
  `stats_v6.bin` and `stats_v5.bin`, so no recognized legacy file survives
  a successful save.
- **Per-version decode constants (design review finding 2):** the legacy
  decoders must not share the current-version constants. `decodeV6` checks
  `STATS_FILE_SIZE_V6` (109) / version `STATS_FILE_VERSION - 1`; `decodeV5`
  checks `STATS_FILE_SIZE_V5` (73) / version `STATS_FILE_VERSION - 2`.
  Deriving them from the bumped current constants would reject every
  legitimate v6/v5 record and turn "fresh start on next save" into "fresh
  start **and delete the v6 file**" — permanent history loss.

### 3.2 Global: v4 → **v5** (195 B → **225 B**)

```
global_stats.bin v5 (225 bytes) = v4 fields unchanged (bytes 0-194) +
  [195-196] sessionWindow.avg      uint16_t LE
  [197-198] sessionWindow.count    uint16_t LE
  [199-223] sessionWindow.samples  uint16_t LE each
  [224]     sessionWindow.pos      uint8_t
```

- `loadFromOpenFile` gains a `== 225` branch mirroring the `== 195` branch
  (common fields + WPM window + session window, **each normalized** after
  read — a torn `count` must never reach `record()`/`trimmedMean()`).
  The v4 branch keeps its own `GLOBAL_STATS_FILE_SIZE_V4` (195) constant
  and version check `GLOBAL_STATS_VERSION - 1`. The forward-format guard
  changes only its constant: `head > GLOBAL_STATS_VERSION` (5).
- v4 (195 B) remains the immediately-previous migration path, session window
  empty. v3 (159 B) unchanged.
- The tmp → verify → `.bak` rotate atomic-save path is size-agnostic and
  needs no change beyond the size constant.

### 3.3 In-memory

`BookReadingStats` gains `SessionWindow sessionWindow;` (+24 B struct,
trivially copyable preserved); `GlobalReadingStats` likewise. Per-device RAM
cost: 24 B (book, loaded per reading session) + 24 B (global) — well within
the lean-device budget.

## 4. Display

Both Avg Session cells go through one new helper in `ReadingStatsUtils`
(host-testable, device-agnostic):

```cpp
// Trimmed-mean session average with the legacy arithmetic-mean fallback:
// window has >= SESSION_MIN_DISPLAY_SAMPLES (4) -> window's running result
// (plain mean up to 4 samples, trimmed beyond); else sessionCount > 0 ->
// legacy all-time mean; else nullopt ("-" cell).
std::optional<uint32_t> avgSessionSeconds(uint16_t windowAvg, uint8_t windowCount,
                                          uint64_t totalReadingSeconds, uint64_t sessionCount);
```

(`uint64_t` for the totals so the legacy mean cannot overflow on the global
record; callers pass their own totals.)

- **Per-book cell** (`BookStatsView::drawPerBookStatsCard`):
  `avgSessionSeconds(stats.sessionWindow.avg, stats.sessionWindow.count,
  stats.totalReadingSeconds, stats.sessionCount)`.
- **Global cell** (`drawGlobalStatsCard`): same with
  `totalSessions`/`totalReadingSeconds`.
- The `-` sentinel contract, `formatDuration` rendering, and cell position
  are unchanged.

**Decision D2 — hybrid fallback with an early-display gate, not a format
flag.** Every existing record migrates with an empty window; showing "-" on
day one for a book with 40 sessions of history would be a regression.
While the window holds fewer than 4 sessions the cell keeps showing the old
arithmetic mean; from the 4th recorded session the window's running result
takes over permanently (plain mean of the first 4, progressively trimmed as
the window fills toward 10). No persisted "migrated" bit is needed: the
gate is purely `count`.

## 5. Clear actions

**User decision D3:** "Clear reading speed" / "Clear global reading speed"
also clear the session window. `clearWpmStats()` keeps its name (the menu
string still says "reading speed") but now clears `wpm` **and**
`sessionWindow`; totals, sessions, dates, buckets, history are untouched.
Comment text updated accordingly. This keeps the one-destructive-action-per-
metric model intact without adding a second menu entry.

**Post-clear display (design review finding 3, accepted behavior):** after a
clear, the Avg Session cell falls back to the all-time arithmetic mean
(D2's `count` gate) until 4 new sessions refill the window — it visibly
reverts to the pre-window value rather than showing "-". This is accepted
deliberately: D3 keeps totals by design, so a number derived from those
totals is honest, and distinguishing "cleared" from "never migrated" would
require a persisted flag that breaks the pure-`count` gate contract.

## 6. Support matrix after this change

| Record | Loaded? | Notes |
|---|---|---|
| Book v7 (134 B, current) | ✅ | Written by this build |
| Book v6 (109 B) | ✅ → v7 in-place | Migrated on first save; legacy file deleted |
| Book v5 (73 B) | ✅ → v7 in-place | Single-hop upgrade on first save; legacy files deleted |
| Book v4 and earlier | ❌ | Unchanged (dropped by design) |
| Global v5 (225 B, current) | ✅ | Written by this build |
| Global v4 (195 B) | ✅ → v5 in-place | Migrated on first save |
| Global v3 (159 B) | ✅ → v4 in-place | Chain preserved |
| Global v1/v2 | ❌ | Unchanged |

(Concretely: a book still on v5 upgrades v5 → v7 in a single save — the
 writer only emits the current version — and both `stats_v6.bin` (if any)
 and `stats_v5.bin` are removed once the v7 write has fully landed.)

## 7. Implementation plan

1. `ReadingStatsUtils.{h,cpp}`: `SessionWindow` (+ constants, `record`,
   `trimmedMean`, `normalize`, `clear`), `avgSessionSeconds` helper.
2. `BookReadingStats.{h,cpp}`: v7 layout (`STATS_FILE_VERSION = 7`,
   `STATS_FILE_SIZE = 134`), `decodeV7`, session-window read/write,
   `recordSession`, extended `clearWpmStats`, candidate names `{v7, v6}`.
3. `GlobalReadingStats.{h,cpp}`: v5 layout (`GLOBAL_STATS_VERSION = 5`,
   225 B), v4 branch gains empty session window, `recordGlobalSession`,
   extended `clearWpmStats`.
4. `EpubReaderActivity`: `sessionPageTurns` counter on the existing
   forward-turn recording block (one counter, no parallel plumbing), reset
   in `onEnter` with the other session state; `onExit` gates the window
   record on `SESSION_MIN_SECONDS && SESSION_MIN_PAGE_TURNS` (§2.2).
5. `BookStatsView.cpp`: both Avg Session cells through `avgSessionSeconds`.
6. Host tests (`test/reading_stats/`): see §8.
7. Docs: this file; `crosspoint-reading-stats` skill reference updates land
   after merge (agent-side, not in the PR).

## 8. Test plan (host, `test/reading_stats/`)

`SessionWindow` (new cases in `ReadingStatsUtilsTest.cpp`):

1. Samples below `SESSION_MIN_SECONDS` are rejected (29 → no sample; 30 →
   recorded).
2. Overflow clamp: a 70 000 s session records as 65 535.
2b. Page-turn engagement gate is enforced at the `EpubReaderActivity` call
   site (`sessionPageTurns >= SESSION_MIN_PAGE_TURNS`); the window itself
   stays duration-only (host tests can't drive the activity, so the gate is
   one `if` there, covered by device test D2 below).
3. Trimmed mean, full window: 10 known samples with 2 planted outliers each
   side → mean of the middle 6, exact value asserted.
4. Partial window ≤ 4 samples → plain mean; 5..17 samples → trimmed mean of
   the middle `count - 4`.
5. `normalize()` invariants: partial window with stale `pos` repaired to
   `pos == count`; full window out-of-range `pos` reset to 0; `avg`
   recomputed, never trusted.
6. Circular wrap: 13 sequential records keep the last 10, `avg` excludes
   the two oldest.

Binary store (`ReadingStatsBinaryStoreTest.cpp`):

7. Book v7 round-trip: all fields including a populated session window.
8. Book v6 → v7 in-place migration: v6 file loads (empty window), save
   writes `stats_v7.bin` and removes `stats_v6.bin`.
9. Book v5 → v7 single-hop upgrade: v5 file loads (empty windows), one save
   writes `stats_v7.bin` and removes both `stats_v6.bin` (if present) and
   `stats_v5.bin`.
10. Global v5 round-trip + v4 → v5 in-place migration (same file path,
    overwrite in place).
11. Forward guard: a 226 B global file with version 6 latches
    `s_blockDestructiveSave` and saves are refused.
12. `clearWpmStats()` zeroes both windows, leaves totals/sessions intact;
    the Avg Session cell falls back to the legacy mean (D2 gate) until the
    window refills to 4 samples — the accepted post-clear behavior of §5.
14. Wire-offset pins: a saved v7 book record has `bytes[109..110] ==
    sessionWindow.avg`, `bytes[111..112] == count`, `bytes[133] == pos`
    (and the global analog at 195/197/224), so a layout regression is
    caught at the file level, not only via round-trip.

Display helper:

13. `avgSessionSeconds`: empty window + zero sessions → `nullopt`; empty
    window + legacy totals → legacy mean; window with 1-3 samples → legacy
    mean (below the display gate); window with 4+ samples → window avg.

## 9. Verification gates (per repo discipline)

- Host tests: full suite green (42 baseline + new cases).
- `pio run -e x4pro` from a **fresh** `.pio/build/x4pro` (RAM ≤ 28 %,
  Flash ≤ 85 %), plus `default`, `sticky`, `papermono`.
- `./bin/clang-format-fix -g` clean; `pio check` defect-gate clean.
- The design doc `§2` constants updated in lockstep with any later change to
  the algorithm (same rule as the WPM window).

## 10. Open items left to future designs

- A "speed history" / session-history view (the window's raw samples are
  persisted but not rendered) — same candidate list as the WPM window's.
- Whether `sessionCount`'s 60 s gate should follow the window's 30 s gate
  once devices accumulate data (D1 keeps them deliberately separate).
