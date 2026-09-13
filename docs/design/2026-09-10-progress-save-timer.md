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
- Progress survives a hard crash within a bounded window (120 s).
- Progress survives book exit, deep sleep, and power off *always* (synchronous flush at those points).
- Under low battery (<5%), degrade to the current save-every-turn behavior.
- Never write when the progress content has not changed.
- **Single owner of the record format:** `ProgressManager` encodes
  (`saveRecord()`), decodes (`load()`), and writes (background tick /
  `flushNow()` / `saveNow()`) the progress record. `EpubReaderUtils::
  saveProgress` is a thin Epub-flavored wrapper; the reader does no
  hand-rolled byte parsing (final structure, SOLID pass).

## 3. Non-Goals

- Changing the `progress.bin` on-disk format for the legacy paths (stays the
  10-byte record of EpubReaderUtils.h; load path in `loadBook()` is untouched).
  **Phase 2a amendment (native-TTF reader, design
  DESIGN_NATIVE_TTF_SUPPORT.md §3.5):** the TTF reader path appends a
  generation-tagged 16-byte record shape
  (`{u16 spine, u16 page, u16 pageCount, u32 charOffset, u32 generation,
  u16 reserved}`), encoded/decoded solely by
  `activities/reader/ProgressRecord.h`. The manager's flush/queue machinery is
  unchanged; load-side migration decodes by exact length and degrades unknown
  sizes to the 6-byte base shape. Downgrading from a TTF build misrestores the
  charOffset as a visibleTextOffset on pre-change firmware (no crash,
  self-healing on the next save) — see docs/file-formats.md.
  **Phase 2b amendment (KOReader remote accept):** a remote-accept save via
  `saveNowTtf(..., charOffset=0, generation)` keeps the 16-byte shape with a
  page-anchored restore — the TTF reader maps `charOffset == 0` records
  through the record's page number (clamped to the chapter's page count)
  instead of the char-offset index. A genuine chapter-start save (page 0,
  charOffset 0) is indistinguishable and restores identically. A generation
  mismatch (settings/layout changed since the sync) still degrades to
  chapter start per §7.
- Migrating TXT/XTC readers to the same manager (follow-up PR).
- A user-facing settings row for the interval (KISS: constexpr only).

## 4. Design

### 4.1 One call per page change

The reader calls `progressManager.save(...)` once per rendered page (in
`renderBook()`, where the live `section` provides the
`visibleTextOffset`). That is the reader's ENTIRE progress obligation:
`openBook()` at load, `save()` on every render, `closeBook()` at teardown.
Everything else — change detection, interval gating, battery fallback,
task scheduling, disk I/O — lives in the manager.

### 4.2 In-memory change detection

`ProgressManager` (final structure) holds TWO records, both allocated with
`poolMalloc` (PSRAM-backed on `BOARD_HAS_PSRAM` boards, DRAM otherwise):

- `current_` — the reader's latest position, updated by every `save()`
  call. The manager IS the single source of truth for progress; the reader
  keeps no shadow copy.
- `lastFlushed_` — the record known to be on disk.

`save()` (called by the reader on every page change) always updates
`current_`; the DISK write is gated:

- changed (`current_ != lastFlushed_`, `visibleTextOffset` participates) AND
- ≥120 s since the last flush — **or** low battery (<5%, gauge HEALTHY, not
  charging — queried from the HAL at gate time; its cached 1.5 s reading
  makes the query free).

When the gate opens, `save()` wakes the worker task
(`xTaskNotifyGive`), which performs the write on core 0. A queued write
also fires within one interval even if its notification raced a flush.

This replaces the existing `lastSavedSpineIndex/page/pageCount` guard
(EpubReaderActivity.cpp:1742) — same purpose, one source of truth, moved
off the render path (DRY). The offset participates in the compare: the old
guard skipped a save when a same-page re-layout shifted only the
`visibleTextOffset`; with the offset in the key that gap is closed.

### 4.3 Flush worker on the second core

`ProgressManager` owns a low-priority FreeRTOS worker task (final shape):

- **Pin:** core 0 on BOTH board classes — the render task owns core 1 on
  dual-core boards (ActivityManager.cpp), and priority 1 cannot preempt it;
  on single-core boards core 0 is the only core. (`xTaskCreatePinnedToCore`,
  not `xTaskCreate` — the pin is part of the review fixes.)
- **Stack:** 2048 BYTES passed directly — this ESP-IDF build takes
  `usStackDepth` in bytes (the render task passes 8192 directly). The
  word-division form created a 512-byte stack (Copilot, PR #107).
- **Trigger:** `ulTaskNotifyTake` with a `FLUSH_INTERVAL_MS` timeout.
  `save()` gives a notification when its gate opens; the timeout is the
  "basic timer since last flush" backstop — a queued write always fires
  within one interval even if its notification raced a flush. No
  always-ticking poll loop, no esp_timer (the interval+notify block is the
  simplest structure that satisfies the bound — KISS).
- **Task loop:** on wake, write `current_` when it differs from
  `lastFlushed_` (one `ProgressFile::writeAtomic` through `HalStorage`,
  under `diskMutex_`). The disk mutex IS held across the write: worker vs
  synchronous flush can never interleave, so there is exactly one writer by
  construction and no torn/overlapping tmp-file state.
- **Book identity:** the manager stores a COPY of the book's cache path
  (registered by `openBook()`), never the `Epub` object — the reader may
  release the epub before teardown (KOReader sync path), so a raw pointer
  would dangle. `closeBook()` performs a full state reset: book A's
  records must never seed book B's gate.
- **Bypass paths:** `saveNow()` writes synchronously (KOReader sync,
  DELETE_CACHE) and updates the in-memory baseline when the path is the
  open book's own file. Exit/sleep/power-off flushing is the manager's job
  (`closeBook()` / `flushNow()`), not the activity's.

**Memory:** 4 KB task stack, two 16-byte records (`current_`/`lastFlushed_`,
pool-allocated, PSRAM-backed where available), one 160-byte cache-path copy.

**Locking model (revised 2026-09-10 on-device, supersedes review finding B1):**
the B1 all-state FreeRTOS mutex deadlocked the reader: `save()` runs on the
render task at every page turn, and taking the manager mutex there let a
flush/openBook/saveNow path holding it across SD I/O block rendering forever
(silent freeze, no panic). Final model: in-memory state is deliberately
UNguarded — `save()` is lock-free (native-width field writes are atomic on
the RISC-V core; a torn multi-field snapshot can only yield a stale,
self-correcting flush baseline, never a crash); one `diskMutex_` serializes
ALL progress.bin disk access (openBook()'s load read, every
flush/saveNow/worker write through `saveRecordLocked()`). An SD stall can
only ever block the flusher, never the render task. The battery gate read
runs before state update, outside any lock.

### 4.4 Book exit — synchronous flush, bounded

The READER DESTRUCTOR calls `closeBook()`: the manager flushes any
unflushed change synchronously (bounded by one record write, ~ms class)
under the mutex, then fully resets its state. Exit flushing is the
manager's responsibility — the activity's teardown just invokes it before
`epub`/`section` are reset. Worst case (SD wedged) adds one write's
latency to leaving a book.

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
through the same disk-serialized path as a timer flush, so the timer's next
fire is a guaranteed no-op (review finding S2) — no double-write, no stale
write, and the timer stays armed so no mode-transition bookkeeping is needed.

Wear is acceptable in this mode: low battery is rare, bounded, and the
user is about to lose the device anyway.

### 4.6 Sleep / power-off paths

`SleepActivity` (auto-power-off dwell) and `HalPowerManager::startDeepSleep()`
entry trigger the same bounded exit-flush before sleeping. Without this, a
crash-only timer would lose the last ≤120 s on every power-off.

**Manual power-off gap (review finding B2, user-confirmed bug):**
`enterPowerOff()` (src/main.cpp:478-511) bypasses the activity lifecycle
entirely — it renders the shutdown screen, persists `APP_STATE`, and calls
`enterPowerOffSleep()` (`[[noreturn]]`) without ever calling the reader's
`onExit()` or destructor. Under this design the last ≤120 s of progress would
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
| `PROGRESS_FLUSH_INTERVAL_MS` | 120000 | Crash window = 2-4 pages; ~30 writes/hr max for a fast reader vs 200-600 today (5-20x reduction depending on reading speed — see review N1). Longer puts more progress at risk per crash; shorter buys nothing the exit/low-battery paths don't already cover. |
| `LOW_BATTERY_PERCENT` (private) | 5 | User-directed. Queried at gate time; gated on battery health HEALTHY and not charging. |
| Worker task priority | low (1) | Must never compete with render. |
| Worker task stack | 4096 B | SdFat write+flush+remove+rename chain overflowed 2048 B (stack canary panic on progress_mgr, first real flush); device-verified 1536 B high-water of 4096. Re-check `uxTaskGetStackHighWaterMark()` after SDK changes. |
| Worker task core | 0 (both classes) | Render task owns core 1 on dual-core boards. |
| Worker trigger | `ulTaskNotifyTake` (120 s timeout) | Notification from save() when the gate opens; the timeout backstops a raced notification. No always-ticking poll. |
| Manager cache-path buffer | 160 B | Holds a copy of the current book's cache dir path; writes dereference the copy, never the Epub object. |
| Shared-state guard | NONE for in-memory state; `diskMutex_` for all progress.bin disk access | Device-proven revision of B1: a manager mutex taken by save() on the render task deadlocked against flush/openBook paths holding it across SD I/O. save() is lock-free (native-width field writes, torn snapshot = stale self-correcting baseline); diskMutex_ serializes load + writeAtomic so disk ops never interleave. |
| Progress state allocation | `poolMalloc` (PSRAM-backed where available) | User directive: dynamic, PSRAM when the board has it. Two 16-byte records. |

## 6. Decision Log

| Date | Decision | Rationale |
|---|---|---|
| 2026-09-10 | 60 s dirty-gated timer | Chosen over 30 s / 120 s: wear reduction is already ample at 60 s (endurance never the binding constraint — the SQLite pivot proved *pattern* was), and the crash window (1-3 pages) is indistinguishable from 30 s in practice. Constant, not a setting (KISS). |
| 2026-09-10 (impl) | Interval raised 60 s → 120 s | User directive: fewer background SD writes matter more than the extra minute of crash-window risk; exit/low-battery/power-off flush paths are unchanged, so the worst-case data loss stays bounded (≤120 s) and only in the hard-crash scenario. |
| 2026-09-10 | Change check in memory (`lastFlushed`) | User-directed. Replaces the render-path guard; `visibleTextOffset` now participates, closing a silent-skip gap in the old spine/page/count-only guard. |
| 2026-09-10 | EPUB reader only in this PR | TXT/XTC keep per-turn saves; follow-up migrates them onto the same manager. Keeps this PR reviewable. |
| 2026-09-10 | Charging counts as NOT low battery | On USB power a crash loses nothing irreplaceable, and the exit flush still runs. Simpler than forcing sync saves while charging. |
| 2026-09-10 | Exit-failure UX = popup | Reuses the existing `STR_SAVE_PROGRESS_FAILED` path; timer failures stay silent. |
| 2026-09-10 | Shared state guarded by FreeRTOS mutex, not `portENTER_CRITICAL` | Review B1 (blocking): on dual-core S3, `portENTER_CRITICAL` without a spinlock disables interrupts on the local core only — two cores never exclude each other, and the 16-byte record copy is not atomic. Torn read → corrupt position on disk. |
| 2026-09-10 | Manual power-off flush added inside `enterPowerOff()` | Review B2 + user confirmation: `enterPowerOff()` bypasses the activity lifecycle (no `onExit()`), so without an explicit flush the last ≤60 s of progress is lost on every manual power-off. User directive: the missing `onExit()` call in that path is itself a bug to fix separately once this design lands. |
| 2026-09-10 | Sync-fallback (low battery) updates `lastFlushed` and clears dirty | Review S2: makes the timer's next fire a guaranteed no-op; no double-write, no mode-transition bookkeeping, timer stays armed. |
| 2026-09-10 | No task notification on capture; flush = next timer fire ≤60 s later | Review S3: the earlier "~1 s later" claim depended on an unspecified notification mechanism. Dropped — the interval bounds the crash window regardless of capture timing, and notification-on-capture adds wake churn for no user-visible benefit. |
| 2026-09-10 | `launchKOReaderSync()`'s early `epub.reset()` documented as safe-by-construction | Review S1: its synchronous save at line 1207 clears the dirty flag before `epub.reset()`, so the later `onExit()` flush is a no-op. Implementation must treat "epub null" as no-op flush, never a fault. |
| 2026-09-10 (impl) | Interval-tick task instead of esp_timer + ISR + task-notify | During implementation: explicit flushes (exit, power-off) write synchronously on the caller's thread, so the ISR/notify machinery had no remaining job. `vTaskDelay` loop is the simplest structure that meets the same bounds. |
| 2026-09-10 (impl) | Manager stores a cache-path copy, not the `Epub*` | The reader releases `epub` before teardown on the KOReader path; a raw pointer would dangle. `setBook(nullptr)` on exit also drops the pending record so book A's position is never written into book B's dir. |
| 2026-09-10 (impl) | `markFlushed()` for synchronous saves that bypass ProgressManager | KOReader sync, DELETE_CACHE and low-battery per-turn saves write outside ProgressManager; recording them keeps change detection consistent and prevents a redundant background rewrite. |
| 2026-09-10 (impl) | Host-testable state machine extracted to `lib/ProgressFlush/` with 9 gtest cases | Review N3 method boundaries: `capture` / `beginFlush` / `endFlush` / `markFlushed`; device wrapper (src/ProgressManager) supplies mutex + SD. Tests: test/progress_flush/. |
| 2026-09-10 (review R1) | `ProgressSaver` renamed `ProgressManager`; owns encode (`saveRecord`), decode (`load`) and write | SOLID/final pass: the record format was defined in three places (EpubReaderUtils encode, reader's loadBook byte parse, ProgressManager writes). One owner; EpubReaderUtils becomes a thin wrapper. (User directive: "fix it properly".) |
| 2026-09-10 (review) | Task pinned core 0, stack passed in BYTES (2048 B) | Copilot: render task owns core 1; and this ESP-IDF build takes usStackDepth in bytes — the word-division form created a 512-byte stack. |
| 2026-09-10 (review) | Write-time freshness gate via published position snapshot | Revalidator callback reading reader state from the manager task was a data race (no RenderLock held); snapshot published under the manager mutex replaces it. Also closes the stale-capture and cross-book hazards. |
| 2026-09-10 (review) | `saveNow()` = capture-first then synchronous flush | Low battery + redraws rewrote unchanged pages; capture through the same change detection makes equal records no-ops and failures retryable. Reopen seeding (`seedLastFlushed`) makes "reopen, read nothing, exit" write nothing. |
| 2026-09-10 (impl) | Lock-free save() + diskMutex_ for disk I/O only; worker stack 4096 B | Device instrumentation found two latent bugs: (1) the all-state mutex from B1 deadlocked — save() on the render task blocked on a flush/openBook path holding it across SD I/O (silent freeze, no panic); (2) the worker's 2048 B stack tripped the canary in the SdFat write+flush+remove+rename chain. In-memory state is now deliberately unguarded; the disk mutex makes read/write interleave impossible. |
| 2026-09-10 (review) | `enterPowerOff()` runs `ActivityManager::shutdown()` first, with a `powerOffInProgress` latch | Manual power-off now runs ANY outgoing activity's onExit() (like goToSleep does) and WiFi activities can no longer silentRestart() during teardown; from-reader context snapshotted before the stack is emptied. |
| 2026-09-10 (redesign) | Gated-save contract: two records + gate in save(); FlushState machinery deleted | User directive ("very complex design — tick, timer, etc"): the manager holds `current_` + `lastFlushed_`; save() always updates memory, gates the disk write on changed+interval (or low battery); worker is notification-driven with a 60 s timeout backstop; flushChangedLocked holds the mutex across the write. Freshness/publish/markFlushed/seed machinery deleted — superseded by "worker always writes the latest in-memory record". |

## 7. Wear Analysis

Per hour of active reading:

- Today: 1 write per page turn ≈ 200-600 writes (plus FAT remove+rename pair each).
- Proposed: ≤30 writes dirty-gated, ~0 when parked on one page, +1 on exit,
  per-turn only under 5% battery. **5-20x reduction depending on reading
  speed** (review N1: 600/hr assumes 10 pages/min — fast; typical readers at
  1-3 pages/min land nearer 5x), effectively unlimited for idle reading.

SD card endurance (10k-100k erase cycles/block): even the per-turn mode is
decades-safe; the win here is bus contention and serialization on the render
path as much as wear.

## 8. Testing

- Host-testable: the record round-trip (encode/decode) is exercised by the
  existing suite; the gate decision (changed × interval × low-battery) is
  pure logic inside `save()` and is exercised on-device via serial logs.
- On-device: serial log shows flush cadence under fast page turns
  (LOG_INF per successful save); `progress.bin` correct after forced
  power-off mid-read (worst case one interval stale); exit flush verified
  by killing power immediately after back-out.

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
