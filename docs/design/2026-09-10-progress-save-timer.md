# Progress-Save Timer — Async Persistence for the EPUB Reader

ISO date: 2026-09-10
Status: Design approved, pending implementation
Scope: EPUB reader progress persistence (`progress.bin`)

## 1. Problem

`EpubReaderActivity::renderBook()` (src/activities/reader/EpubReaderActivity.cpp:1742-1749)
calls `saveProgress()` synchronously after every page render whose position
changed. The write takes the `storageMutex` inline with the page-turn hot path:
serialization cost, SD I/O latency, and SD wear proportional to page-turn rate.
A fast reader generates 200-600 writes per hour against a file whose content is
usually identical to the last write apart from one page number.

The write itself is already crash-safe and cheap (10 bytes, tmp+rename via
`ProgressFile::writeAtomic`, src/activities/reader/ProgressFile.h:32), but the
*frequency* is the problem.

## 2. Goals

- Zero SD I/O on the page-turn hot path under normal battery.
- Progress survives a hard crash within a bounded window (60 s).
- Progress survives book exit, deep sleep, and power off *always* (synchronous flush at those points).
- Under low battery (<5%), degrade to the current save-every-turn behavior.
- Never write when the progress content has not changed.

## 3. Non-Goals

- Changing the `progress.bin` on-disk format (stays the 10-byte record of
  EpubReaderUtils.h; load path in `loadBook()` is untouched).
- Migrating TXT/XTC readers to the same saver (follow-up PR).
- A user-facing settings row for the interval (KISS: constexpr only).

## 4. Design

### 4.1 Capture at page turn, write later

`pageTurn()` (or the equivalent position-mutating paths: `skipPages()`,
`jumpToPercent()`, chapter jumps) calls `captureCandidate()` on the new
position. The capture must happen *while the live `section` exists*, because
the `visibleTextOffset` lookup (`section->getVisibleTextOffsetForPage()`)
requires it. Capture is pure memory: copy the 16-byte position record and mark
dirty if changed. No mutex, no SD.

### 4.2 In-memory change detection

The saver holds `lastFlushed = {spine, page, pageCount, visibleTextOffset}`
(16 bytes, mirroring the 10-byte on-disk record). `captureCandidate()`
compares the candidate against `lastFlushed`:

- identical -> not dirty; the timer fires on an empty flag and performs zero
  SD I/O (a reader parked on one page writes nothing, indefinitely);
- different -> dirty; the flush task will write on its next wake.

On a successful flush, `lastFlushed` is updated to the written position. The
exit flush goes through the same compare, so "opened, read nothing, exited"
writes nothing.

This replaces the existing `lastSavedSpineIndex/page/pageCount` guard
(EpubReaderActivity.cpp:1742) — same purpose, one source of truth, moved off
the render path (DRY). The offset now participates in the compare: the old
guard skipped a save when a same-page re-layout shifted only the
`visibleTextOffset`; with the offset in the key that gap is closed.

### 4.3 Flush worker on the second core

A small `ProgressSaver` (src/ProgressSaver.cpp) owns a FreeRTOS task:

- **Pin:** core 1 on dual-core boards (x4pro, sticky); core 0 at low priority
  on the single-core C3. Pin chosen at creation from `portNUM_PROCESSORS`.
- **Trigger (simplified from the esp_timer sketch):** the task itself loops on
  `vTaskDelay(FLUSH_INTERVAL_MS)` and flushes when dirty. The original
  esp_timer + `xTaskNotifyFromISR` design assumed the explicit flushes would
  signal the task; in the final shape the explicit paths (book exit, power
  off) write synchronously on the caller's thread instead, so the ISR/notify
  machinery had no remaining job. An interval tick with no interrupt context
  is the simplest structure that satisfies the design (KISS).
- **Task loop:** on wake, if dirty, copy the pending record out under the
  shared-state mutex, then one `ProgressFile::writeAtomic` through
  `HalStorage` (its mutex serializes with the main task's other SD access)
  with the mutex NOT held. Update `lastFlushed` on success; leave dirty set
  on failure so the next tick retries.
- **Book identity:** the saver stores a COPY of the book's cache path
  (`setBook()`), never the `Epub` object — the reader may release the epub
  before teardown (KOReader sync path), so a raw pointer would dangle.
  `setBook(nullptr)` on book exit drops any pending record: a record
  captured for book A must never be written into book B's cache dir.
- **Bypass-path sync:** synchronous saves that bypass the saver (KOReader
  sync, DELETE_CACHE, low-battery per-turn saves) call `markFlushed()` so
  the saver's change detection stays consistent and its next tick is a
  no-op.

**Memory:** ~2 KB task stack, one 16-byte pending record, one 16-byte
`lastFlushed` record, one 160-byte cache-path copy, static (no heap after
task creation at boot).

**Cross-core protection (review finding B1):** on the dual-core S3,
`portENTER_CRITICAL()` without a spinlock disables interrupts on the local
core only and is NOT a mutual-exclusion pair across cores — a torn 16-byte
read was possible. The shared state (dirty flag + pending record +
`lastFlushed`) is therefore guarded by a dedicated FreeRTOS mutex
(`xSemaphoreCreateMutex()`), taken by both `captureCandidate()` (reader task)
and the flush loop (saver task) around the read-modify-update of the shared
record. On the single-core C3 the same mutex is simply correct too, so one
code shape serves both. The critical section never wraps SD I/O — the mutex
is held only for the memory copy.

### 4.4 Book exit — synchronous flush, bounded

`onExit()` (and the destructor path that already re-saves footnote origin,
EpubReaderActivity.cpp:188-191) captures the final position and requests an
immediate flush, then waits up to EXIT_FLUSH_TIMEOUT_MS (2000) for the task
to complete before `epub`/`section` are reset. The async design exists only
for mid-reading; at exit the reader state is being destroyed, so this one
save is on-path by design. Worst case (SD wedged) adds ≤2 s to leaving a
book.

The `epub` reference needed for the cache path must remain valid during this
wait — the flush is issued before any `.reset()` in the teardown sequence.

**KOReader sync exception (review finding S1):** `launchKOReaderSync()`
(EpubReaderActivity.cpp:1185-1239) calls `saveProgress()` synchronously at
line 1207 and then `epub.reset()` at line 1231 BEFORE
`activityManager.replaceActivity()` — so the later `onExit()` runs with
`epub == nullptr` and its flush is a no-op. This is safe by construction:
the line-1207 save both persisted the position and cleared the dirty flag,
so there is nothing left for a teardown flush to write. §4.4's teardown-order
promise applies to the normal exit paths, not this one; the implementation
must treat "epub already null" as a no-op flush, never a fault.

### 4.5 Low battery — per-turn synchronous fallback

When `powerManager.getBatteryPercentage() < LOW_BATTERY_PERCENT (5)` and
`getBatteryHealthState() == HEALTHY` (HalPowerManager.h:53-63 — the health
gate exists precisely so a stale gauge read cannot fake a low-battery
signal), `pageTurn()`'s capture degrades to the current synchronous
`saveProgress()` call. Above 5%: timer mode only.

The synchronous fallback **updates `lastFlushed` and clears the dirty flag**
through the same shared-state mutex as a timer flush, so the timer's next
fire is a guaranteed no-op (review finding S2) — no double-write, no stale
write, and the timer stays armed so no mode-transition bookkeeping is needed.

Wear is acceptable in this mode: low battery is rare, bounded, and the
user is about to lose the device anyway.

### 4.6 Sleep / power-off paths

`SleepActivity` (auto-power-off dwell) and `HalPowerManager::startDeepSleep()`
entry trigger the same bounded exit-flush before sleeping. Without this, a
crash-only timer would lose the last ≤60 s on every power-off.

**Manual power-off gap (review finding B2, user-confirmed bug):**
`enterPowerOff()` (src/main.cpp:478-511) bypasses the activity lifecycle
entirely — it renders the shutdown screen, persists `APP_STATE`, and calls
`enterPowerOffSleep()` (`[[noreturn]]`) without ever calling the reader's
`onExit()` or destructor. Under this design the last ≤60 s of progress would
be lost on every manual power-off. This design will add a synchronous bounded
flush inside `enterPowerOff()` before the noreturn path, using the
reader's last captured position (see §9 Related fixes).

### 4.7 Failure UX

- Timer flush failure: LOG_ERR only. A background failure must never
  interrupt reading; the dirty flag keeps the retry alive.
- Exit flush failure: LOG_ERR + the existing `STR_SAVE_PROGRESS_FAILED`
  popup (already wired via `pendingSyncSaveError`, EpubReaderActivity.cpp:1421-1425).

## 5. Constants

| Constant | Value | Rationale |
|---|---|---|
| `PROGRESS_FLUSH_INTERVAL_MS` | 60000 | Crash window = 1-3 pages; ~60 writes/hr max for a fast reader vs 200-600 today (3-10x reduction depending on reading speed — see review N1). Longer doubles wear saving nobody needs; shorter buys nothing the exit/low-battery paths don't already cover. |
| `LOW_BATTERY_PERCENT` | 5 | User-directed. Gated on battery health HEALTHY. |
| `EXIT_FLUSH_TIMEOUT_MS` | 2000 | Bounds the worst-case book-exit latency if SD is wedged. |
| Saver task priority | low (1) | Must never compete with render. |
| Saver task stack | 2048 B | Record build + HalStorage call; no recursion. Verify with `uxTaskGetStackHighWaterMark()` on first device test (review N2). |
| Saver task core | 1 (dual-core) / 0 (single-core, low prio) | Off the render core. |
| Saver cache-path buffer | 160 B | Holds a copy of the current book's cache dir path; flushes dereference the copy, never the Epub object. |
| Shared-state guard | FreeRTOS mutex | Review B1: `portENTER_CRITICAL` without a spinlock is per-core on the S3 and not a cross-core exclusion pair. One mutex shape serves both S3 and C3. |

## 6. Decision Log

| Date | Decision | Rationale |
|---|---|---|
| 2026-09-10 | 60 s dirty-gated timer | Chosen over 30 s / 120 s: wear reduction is already ample at 60 s (endurance never the binding constraint — the SQLite pivot proved *pattern* was), and the crash window (1-3 pages) is indistinguishable from 30 s in practice. Constant, not a setting (KISS). |
| 2026-09-10 | Change check in memory (`lastFlushed`) | User-directed. Replaces the render-path guard; `visibleTextOffset` now participates, closing a silent-skip gap in the old spine/page/count-only guard. |
| 2026-09-10 | EPUB reader only in this PR | TXT/XTC keep per-turn saves; follow-up migrates them onto the same saver. Keeps this PR reviewable. |
| 2026-09-10 | Charging counts as NOT low battery | On USB power a crash loses nothing irreplaceable, and the exit flush still runs. Simpler than forcing sync saves while charging. |
| 2026-09-10 | Exit-failure UX = popup | Reuses the existing `STR_SAVE_PROGRESS_FAILED` path; timer failures stay silent. |
| 2026-09-10 | Shared state guarded by FreeRTOS mutex, not `portENTER_CRITICAL` | Review B1 (blocking): on dual-core S3, `portENTER_CRITICAL` without a spinlock disables interrupts on the local core only — two cores never exclude each other, and the 16-byte record copy is not atomic. Torn read → corrupt position on disk. |
| 2026-09-10 | Manual power-off flush added inside `enterPowerOff()` | Review B2 + user confirmation: `enterPowerOff()` bypasses the activity lifecycle (no `onExit()`), so without an explicit flush the last ≤60 s of progress is lost on every manual power-off. User directive: the missing `onExit()` call in that path is itself a bug to fix separately once this design lands. |
| 2026-09-10 | Sync-fallback (low battery) updates `lastFlushed` and clears dirty | Review S2: makes the timer's next fire a guaranteed no-op; no double-write, no mode-transition bookkeeping, timer stays armed. |
| 2026-09-10 | No task notification on capture; flush = next timer fire ≤60 s later | Review S3: the earlier "~1 s later" claim depended on an unspecified notification mechanism. Dropped — the interval bounds the crash window regardless of capture timing, and notification-on-capture adds wake churn for no user-visible benefit. |
| 2026-09-10 | `launchKOReaderSync()`'s early `epub.reset()` documented as safe-by-construction | Review S1: its synchronous save at line 1207 clears the dirty flag before `epub.reset()`, so the later `onExit()` flush is a no-op. Implementation must treat "epub null" as no-op flush, never a fault. |
| 2026-09-10 (impl) | Interval-tick task instead of esp_timer + ISR + task-notify | During implementation: explicit flushes (exit, power-off) write synchronously on the caller's thread, so the ISR/notify machinery had no remaining job. `vTaskDelay` loop is the simplest structure that meets the same bounds. |
| 2026-09-10 (impl) | Saver stores a cache-path copy, not the `Epub*` | The reader releases `epub` before teardown on the KOReader path; a raw pointer would dangle. `setBook(nullptr)` on exit also drops the pending record so book A's position is never written into book B's dir. |
| 2026-09-10 (impl) | `markFlushed()` for synchronous saves that bypass the saver | KOReader sync, DELETE_CACHE and low-battery per-turn saves write outside the saver; recording them keeps change detection consistent and prevents a redundant background rewrite. |
| 2026-09-10 (impl) | Host-testable state machine extracted to `lib/ProgressFlush/` with 9 gtest cases | Review N3 method boundaries: `capture` / `beginFlush` / `endFlush` / `markFlushed`; device wrapper (src/ProgressSaver) supplies mutex + SD. Tests: test/progress_flush/. |

## 7. Wear Analysis

Per hour of active reading:

- Today: 1 write per page turn ≈ 200-600 writes (plus FAT remove+rename pair each).
- Proposed: ≤60 writes dirty-gated, ~0 when parked on one page, +1 on exit,
  per-turn only under 5% battery. **3-10x reduction depending on reading
  speed** (review N1: 600/hr assumes 10 pages/min — fast; typical readers at
  1-3 pages/min land nearer 3x), effectively unlimited for idle reading.

SD card endurance (10k-100k erase cycles/block): even the per-turn mode is
decades-safe; the win here is bus contention and serialization on the render
path as much as wear.

## 8. Testing

- Host-testable: the change-detection logic (candidate vs `lastFlushed`)
  extracted into `lib/` per the host-test-lib pattern (references/host-test-lib-pattern.md);
  the dirty/latch/retry state machine is pure C++ (review N3: expose explicit
  method boundaries — `capture(record)`, `shouldFlush()`, `markFlushed(record)` —
  so the state machine is testable without mocking FreeRTOS primitives).
- On-device: serial log shows flush cadence under fast page turns; `progress.bin`
  correct after forced power-off mid-read (worst case one interval stale);
  exit flush verified by killing power immediately after back-out.

## 9. Related fixes (outside this PR)

| Fix | Origin | Timing |
|---|---|---|
| `enterPowerOff()` must run the current activity's `onExit()` (or at minimum the reader's exit flush) before `enterPowerOffSleep()` — the activity lifecycle is currently bypassed entirely on manual power-off, which also skips today's footnote-origin re-save at EpubReaderActivity.cpp:188-191 and the READING_STATS session commit. | Review B2 + user confirmation ("that's a bug") | Separate PR once this design lands |

## 10. External review pass (applied 2026-09-10)

Reviewed by `opencode/big-pickle` (verdict: REVISE, 2 blockers / 3 should-fix /
3 nits; all file:line citations in this doc verified against source). Findings
and their disposition:

- **B1** `portENTER_CRITICAL` is per-core on S3, not a cross-core exclusion →
  shared state now guarded by a FreeRTOS mutex (§4.3, §5, decision log).
- **B2** manual power-off bypasses `onExit()` → explicit flush added to the
  design (§4.6); the underlying lifecycle bug recorded in §9 as a separate fix.
- **S1** KOReader sync releases `epub` before teardown → documented as
  safe-by-construction; implementation must no-op on null epub (§4.4).
- **S2** sync-fallback vs timer ambiguity → fallback updates `lastFlushed` and
  clears dirty (§4.5, decision log).
- **S3** "prompt flush" claim unspecified → replaced with explicit
  next-timer-fire semantics (§4.3, decision log).
- **N1** 10x wear claim overstated → corrected to 3-10x (§5, §7).
- **N2** 2 KB task stack unverified → runtime `uxTaskGetStackHighWaterMark()`
  check added to constants table (§5).
- **N3** host-test decomposition → explicit method-boundary note in §8.

Full review text: `/tmp/bigpickle_progress_review.md` (session artifact, not
committed).
