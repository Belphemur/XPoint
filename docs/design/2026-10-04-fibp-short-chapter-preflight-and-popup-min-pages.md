---
title: Short-chapter prefetch fire + minimum-pages popup gate
date: 2026-10-04
status: approved-for-implementation
owner: Antoine Aflalo (Belphemur)
target_env: x4pro (ESP32-S3, BOARD_HAS_PSRAM + CROSSPOINT_TTF_READER + CROSSPOINT_FONT_BACKEND_FT)
related: docs/design/2026-09-18-freetype-backend-as-built.md §7 (FIBP prefetch policy)
authors:
  - design: "Antoine Aflalo (Belphemur), Hermes Agent"
  - implementation: pi
---

# Short-chapter prefetch fire + minimum-pages popup gate

## 1. Problem statement

Two related but distinct owner-reported blockings on x4pro:

**(A) The 2-page repro.** Reading a 2-page chapter: turning past its end blocks on
the Indexing popup for the next chapter. Closing and reopening the book "fixed"
it (the idle prefetcher then had time to drain).

**(B) The minimum-pages ask.** "It's okay to see the indexing message but it
should let me read after reaching the minimum of pages, I think it's 5. Like
when opening the book after changing the font." Font changes on x4pro are the
TTF path, so this is the inline TTF cache-miss rebuild, not the legacy Section
path.

## 2. Root-cause analysis

### 2.A — FIBP prefetch trigger under-fires on short chapters

The background prefetch worker exists and is correct (`FibpPrefetchWorker` in
`src/activities/reader/FibpPrefetchWorker.{h,cpp}`, active only when
`BOARD_HAS_PSRAM && CROSSPOINT_TTF_READER && CROSSPOINT_FONT_BACKEND_FT`).

Its trigger lives in **`src/activities/reader/FibpPrefetchPolicy.h`**:

```cpp
constexpr uint8_t kPrefetchRemainingPercent = 10;
inline bool shouldPrefetchNext(const uint16_t page, const uint16_t pageCount) {
  if (pageCount == 0) return false;
  const uint32_t thresholdPages = (pageCount * 10 + 99) / 100; // ceil
  const uint32_t remaining = pageCount > page ? pageCount - page : 0;
  return remaining <= thresholdPages;
}
```

It enqueues the next chapter only once the reader is inside the **last 10%** of
the current chapter. For a 2-page chapter `ceil(2*10/100)=1`, so it fires at
**page 1 of 2** — one turn from the spine boundary. The worker then has <1s of
wall-clock budget to index a chapter that takes seconds; it cannot finish before
the reader arrives. The Indexing popup blocks on the turn.

Reopen works because the book opens on page 0 of the cached chapter and the
idle worker drains the next-chapter queue during the open + first render.

The 10% rule is **correct for long chapters** (don't waste SD writes/battery
indexing a chapter the reader may never reach — the device-soak pathology from
whole-book prefetch that PR #153 fixed) and **wrong for short ones**: there is
no reading time left to hide the index behind.

The worker's own dedup (`lastPrefetchFiredSpine_`) and the
`notifyGeneration` spawn-re-arm gate (line 386-393) only fire when *a* threshold
event has occurred — so the fix is purely in the threshold predicate.

### 2.B — The Indexing popup has no minimum-pages-to-clear gate

The inline cache-miss rebuild (TTF path) is at
`src/activities/reader/EpubReaderActivity.cpp:2287-2371`:

```
showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate)
                                 || target > BUILD_POPUP_PAGE_THRESHOLD);
```

`BUILD_POPUP_PAGE_THRESHOLD = 20` decides whether to *show* the popup at all.
Once shown, the popup is cleared via `pagesUntilFullRefresh`, which is driven by
the **e-ink refresh cadence** (`SETTINGS.getRefreshFrequency()` and the
`pagesUntilFullRefresh--` decrements at lines 3690 and 4299) — **not** by build
progress. There is no condition that dismisses the Indexing popup once N pages
are built, so the reader sits on the popup until the entire (or targeted)
chapter finishes building.

Owner wants: dismiss once >= 5 pages are laid out, so they can start reading
while the rest continues.

## 3. Scope

- **Target env**: `x4pro` (the only env the owner uses and the only one where
  the TTF/FIBP path is reachable).
- **In scope**: (A) the `shouldPrefetchNext` predicate; (B) a minimum-pages gate
  on the inline TTF rebuild popup path.
- **Out of scope**: the legacy non-TTF Section path, `metalio_eink4` (PSRAM but
  no `CROSSPOINT_TTF_READER`), restoring whole-book prefetch, changing
  `kPrefetchLookaheadSpines` (stays 1), any user-facing setting, device flashing.

## 4. Build-flag reachability (verified against `platformio.ini`, 2026-10-04)

`FibpPrefetchWorker` is an inert stub unless all three flags are set. Only these
envs are affected by (A):

| env | PSRAM | TTF_READER | FONT_BACKEND_FT | worker |
|---|---|---|---|---|
| x4pro | Y | Y | Y | ENABLED |
| x4pro-gh_release | Y | Y | Y | ENABLED |
| x4pro-gh_release_rc | Y | Y | Y | ENABLED |
| x4c | Y | Y | Y | ENABLED |
| x4c-gh_release | Y | Y | Y | ENABLED |

**(B)** is in `EpubReaderActivity.cpp` and compiles on ALL envs, but is only
reached on the TTF path (`ttf_` non-null → `CROSSPOINT_TTF_READER=1`).

**Report-only**: `x4c-gh_release_rc` has `-DBOARD_HAS_PSRAM` (line 578) but is
missing `CROSSPOINT_TTF_READER=1` and `CROSSPOINT_FONT_BACKEND_FT=1`, while its
sibling `x4c-gh_release` (line 543) has all three. Looks like an omission; flag
in final report, do not fix here.

## 5. Design

### 5.A — Immediate prefetch for short current chapters

Add a second threshold to `FibpPrefetchPolicy.h` and fold both rules into
`shouldPrefetchNext`:

```cpp
// A chapter with fewer than this many pages is over before any %-based trigger
// can complete the next chapter's index in time — enqueue the next chapter
// immediately on chapter entry (page 0). Chapters at/above this length keep the
// 10%-remaining rule so we don't burn SD writes indexing chapters the reader
// may never reach. (2026-10-04, owner repro: 2-page chapter blocked on
// Indexing popup for the next chapter.)
constexpr uint16_t kShortChapterImmediatePrefetchPages = 10;

inline bool shouldPrefetchNext(const uint16_t page, const uint16_t pageCount) {
  if (pageCount == 0) return false;          // unknown length: never fire
  if (pageCount < kShortChapterImmediatePrefetchPages) return true;  // short: fire at entry
  const uint32_t thresholdPages = (static_cast<uint32_t>(pageCount) * kPrefetchRemainingPercent + 99) / 100;
  const uint32_t remaining = pageCount > page ? static_cast<uint32_t>(pageCount - page) : 0;
  return remaining <= thresholdPages;         // long: 10%-remaining rule
}
```

**Why this is correct and minimal:**

- The caller (`EpubReaderActivity::updateFibpWorker`, line 3960) already passes
  the current chapter's `pageCount` via `notifyChapterProgress(spine, page, pageCount)`.
  No new plumbing.
- The function is pure and already host-tested in
  `test/fibp_prefetch_policy/`. No call-site branch needed — `notifyChapterProgress`
  reports raw position, the policy decides. DRY: one predicate source of truth.
- The worker's dedup (`lastPrefetchFiredSpine_`) and spawn-re-arm
  (`notifyGeneration` line 386 `fired` check) already prevent re-firing per
  page turn within the same spine — a short chapter fires once on entry.
- Long-chapter behavior is byte-for-byte the old rule (`pageCount < 10` is the
  only new branch; at `pageCount == 10` the `remaining <= 1` test for page 9
  differs from "fire at entry" only at the single entry page — see the boundary
  test). The 10%-remaining SD-wear protection from PR #153 is intact.
- `pageCount == 0` (unknown — e.g. a not-yet-laid-out chapter) stays `false`:
  an unknown length is never treated as short.

**Edge cases:**
- Single-page chapter (`pageCount == 1`): already fired at entry by the old
  ceil path (`ceil(1*10/100)=1`, remaining 1 <= 1) — stays true. No regression.
- Boundary: at exactly 10 pages, entry (page 0) yields `remaining=10`,
  `thresholdPages=ceil(1.0)=1` → false. The old rule also fires at page 9. The
  new rule does NOT fire at entry for a 10-page chapter — that is the intended
  line: 10 is "long enough to wait". Tests assert 9 vs 10 explicitly.

### 5.B — Minimum-pages gate to dismiss the Indexing popup

Add a constexpr and a pure predicate, plus a build-progress check in the inline
rebuild loop:

```cpp
// In EpubReaderActivity.h:
static constexpr int MIN_PAGES_TO_CLEAR_POPUP = 5;
static constexpr bool shouldClearBuildPopup(const int pagesBuilt) {
  return pagesBuilt >= MIN_PAGES_TO_CLEAR_POPUP;
}
```

In the inline rebuild loop (`EpubReaderActivity.cpp:2355-2369`), after each
`buildSomeMore(BUILD_PAGES_PER_CHUNK)` chunk, if the popup is up and the section
has built >= `MIN_PAGES_TO_CLEAR_POPUP` pages, dismiss it:

```cpp
while (!section->isBuildComplete() && ...) {
  ...
  if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) { ...; return; }
  if (buildPopupPending_wasShown && shouldClearBuildPopup(section->pageCount)) {
    GUI.clearPopup(renderer);   // stop drawing STR_INDEXING
    buildPopupPending = false;
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency(); // hand back to cadence
  }
}
```

**Design notes:**
- `section->pageCount` is the built page count (it grows as chunks land — see
  `Section::buildSomeMore` writes `builtPageCount_`). It is the live read of
  how many pages the build has produced, exactly what the gate needs.
- This is a **display** dismissal only — the build loop keeps running (the
  `while (!section->isBuildComplete())` continues) via the async-display
  overlap path at line 4295-4316 (`prefetchNextChapterDuringDisplay` +
  `section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)`), so the rest of the
  chapter finishes building during display and the reader can turn the page
  they can already see.
- Keeps `pagesUntilFullRefresh` for refresh cadence only (don't conflate the two
  counters). The popup-clear is a separate, build-progress condition.
- `buildPopupPending` is the existing "has the popup been shown / is it queued"
  latch — reuse it rather than adding a parallel flag (DRY).

**Edge cases:**
- A 1-page or 2-page chapter: the build completes before reaching 5 pages in
  most cases; `isBuildComplete()` wins the loop and the popup is cleared on
  completion as before. If a build is slow and crosses 5 pages, the popup
  clears early — the owner said that is acceptable ("it's okay to see the
  indexing message but it should let me read after reaching the minimum").
- `BUILD_POPUP_DEADLINE_MS` (1000ms) still governs the *delayed* show
  (`buildPopupPending` → shown after the deadline); this gate only dismisses
  once shown. No interaction.

## 6. Constants table

| Symbol | Location | Value | Rationale |
|---|---|---|---|
| `kPrefetchRemainingPercent` | FibpPrefetchPolicy.h | 10 | Unchanged. Last-10% trigger for long chapters. |
| `kShortChapterImmediatePrefetchPages` | FibpPrefetchPolicy.h (NEW) | 10 | Chapters shorter than this have no reading-time budget to hide the next-chapter index; fire at entry. |
| `BUILD_POPUP_PAGE_THRESHOLD` | EpubReaderActivity.h | 20 | Unchanged. Decides whether to show the Indexing popup at all. |
| `MIN_PAGES_TO_CLEAR_POPUP` | EpubReaderActivity.h (NEW) | 5 | Owner-specified minimum so they can start reading while the rest builds. |
| `BUILD_POPUP_DEADLINE_MS` | EpubReaderActivity.h | 1000 | Unchanged. Delayed show of the popup. |
| `BACKGROUND_BUILD_PAGES_PER_TICK` | EpubReaderActivity.h | 2 | Unchanged. |

## 7. Test plan (host, `test/fibp_prefetch_policy/`)

(A) `shouldPrefetchNext`:
- `EXPECT_TRUE(shouldPrefetchNext(0, 2))` — the 2-page repro, fires at entry.
- Boundary: `EXPECT_FALSE(shouldPrefetchNext(0, 10))` (10 is "long" — no entry fire),
  `EXPECT_TRUE(shouldPrefetchNext(0, 9))` (9 is "short" — entry fire).
- `EXPECT_FALSE(shouldPrefetchNext(0, 0))` — unknown length never fires.
- `EXPECT_FALSE(shouldPrefetchNext(0, 200))` — long chapter does not fire at entry;
  existing 10%-crossing tests (page 180/199 of 200 fire) stay green.
- 1-page chapter still fires at entry (existing, no regression).

(B) `shouldClearBuildPopup` (new test, pure function):
- `EXPECT_FALSE(shouldClearBuildPopup(0))`, `EXPECT_FALSE(shouldClearBuildPopup(4))`,
  `EXPECT_TRUE(shouldClearBuildPopup(5))`, `EXPECT_TRUE(shouldClearBuildPopup(57))`
  — 5 is the minimum built page count to dismiss.

## 8. Verification gates

```bash
cd _worktrees/fibp-short-chapter   # this branch
cd test && cmake -B build -DCROSSPOINT_BUILD_BENCH=OFF && cmake --build build && ctest --test-dir build --output-on-failure
~/.platformio/penv/bin/pio run -e x4pro        # (A)+(B) TTF path
~/.platformio/penv/bin/pio run -e x4c          # second enabled class
~/.platformio/penv/bin/pio run -e metalio_eink4  # PSRAM-but-inert (legacy Section path still builds)
~/.platformio/penv/bin/pio run -e default      # no-PSRAM floor
./bin/clang-format-fix -g
git diff --exit-code         # formatter idempotent
./bin/cppcheck-check
```

## 9. Documentation lockstep

Update `docs/design/2026-09-18-freetype-backend-as-built.md` §7 ("Prefetch
trigger") to describe the short-chapter immediate fire, and append a dated
decision-log row here (this doc) referencing it. Rewrite (do not append-only
over) the §7 prose that states the trigger is purely percentage-based.

## 10. On-device confirmation (owner)

- (A): `[PREF] indexed <href> pages=... ms=... hwm=...` should appear for the
  *next* chapter before the turn into a short chapter — no Indexing popup on the
  turn. Reproduce: a 2-page chapter, turn past its end.
- (B): on font change (cache-miss chapter), the Indexing popup should clear
  after ~5 pages build (a few seconds) and let the page render.

## 11. Decision log (append-only)

| Date | Decision | Rationale |
|---|---|---|
| 2026-09-19 | Prefetch plan capped to the NEXT chapter (`kPrefetchLookaheadSpines = 1`), triggered at the last 10% of the current chapter. | Whole-book prefetch measured as device-soak pathology (SD-write pressure, task-WDT abort at spine 65 of a 177-spine run); the percentage trigger keeps indexing out of chapters the reader may never open. Recorded in `2026-09-18-freetype-backend-as-built.md` §7. |
| 2026-10-04 | Chapters **under 10 pages** request the prefetch on **entry**; chapters of 10+ keep the last-10% trigger. `pageCount == 0` still never triggers. | Owner device repro: a 2-page chapter blocked on the Indexing popup for the next chapter; the 10% threshold fires at page 1 of 2, one turn from the boundary, and the worker cannot index a multi-second chapter in one page-turn of wall clock. Reopening the book masked it by letting the idle worker drain. Short chapters have no reading time left to hide the index behind, so the SD-wear argument behind the percentage rule does not apply to them. |
| 2026-10-04 | The Indexing popup clears once **5 pages** of the current chapter's cache-miss rebuild are laid out. | Owner: "it's okay to see the indexing message but it should let me read after reaching the minimum of pages. Like when opening the book after changing the font." The popup was dismissed only by the e-ink refresh cadence, never by build progress, so a font-change reopen blocked for the whole chapter. Remaining pages keep building via the existing async-display overlap. |

### Implementation-time notes

- The rebuild loop already tracks build progress in `section->pageCount`, so the
  min-pages gate reuses it plus the existing `buildPopupPending` latch — no new
  field and no parallel state. The popup is stateless paint (`BaseTheme::drawPopup`
  draws, nothing tracks it), so clearing the latch stops the redraw and
  `pagesUntilFullRefresh` goes back to the refresh cadence.
- The ceil-threshold host test had to move: its `pageCount` values of 5 and 3 are
  short chapters under the new rule, so they now fire at entry. Ceil is still
  asserted, on long chapters where the remaining-page branch is reachable.
- Not changed: `kPrefetchLookaheadSpines`, the single-writer claim/resume handoff,
  the takeover grace, and the worker's dedup — an entry-time fire on a short
  chapter still enqueues exactly once.
