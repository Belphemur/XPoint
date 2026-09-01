# Design: Kindle-Style WPM Reading Speed with Word-Count Plumbing

## Problem Statement

Current `recordForwardPageRead(uint32_t seconds)` only receives elapsed time → can only compute seconds-per-page median.
Kindle's algorithm uses **words-per-minute (WPM)** with a trimmed mean, requiring `words_on_page` at page-turn time.
No word-count mechanism exists in the EPUB rendering pipeline today.

## Scope

Plumb word counts from EPUB parser → Section pagination → page-turn callback → reading stats.
Then implement Kindle-style trimmed-mean WPM tracker in `BookReadingStats`.

---

## 1. EPUB Pipeline Changes

### 1.1 `Paragraph` class (`lib/Epub/Epub/blocks/TextBlock.h`)
```cpp
// Add method to count words in paragraph text
uint16_t wordCount() const;
```
- Split `text` on Unicode whitespace + punctuation boundaries
- Cache result (paragraphs are immutable after parsing)
- Return `uint16_t` (max ~65k words/paragraph — safe)

### 1.2 `Section` pagination (`lib/Epub/Epub/Section.cpp`)
- `makePages()` / `loadPage()` already iterate paragraphs to build pages
- Add `uint16_t wordsOnPage` accumulator in pagination loop
- Store in `EpubPage` or pass directly to `onPageComplete`

### 1.3 `onPageComplete` callback signature
```cpp
// Current:
using PageCompleteCallback = std::function<void(uint16_t paragraphIndex, bool isLastPage)>;

// New:
using PageCompleteCallback = std::function<void(uint16_t paragraphIndex, bool isLastPage, uint16_t wordsOnPage)>;
```

### 1.4 `EpubReaderActivity::onPageComplete`
```cpp
// Current:
void onPageComplete(uint16_t paragraphIndex, bool isLastPage) { ... }

// New:
void onPageComplete(uint16_t paragraphIndex, bool isLastPage, uint16_t wordsOnPage) {
    stats.recordForwardPageRead(currentPageReadingSeconds, wordsOnPage);
    ...
}
```

---

## 2. BookReadingStats Changes

### 2.1 Binary layout cleanup (v5 → v6)
```cpp
// v5 record (73 bytes) — byte-compatible with crossink's stats_v5.bin:
//   [0]       version (= 5)
//   [1-2]     sessionCount              uint16_t LE
//   [3-6]     totalReadingSeconds       uint32_t LE
//   [7-10]    totalPagesTurned          uint32_t LE
//   [11]      isCompleted               uint8_t
//   [12-15]   RESERVED (was avgSecondsPerForwardPage / paceSampleCount)
//   [16]      flags bit0=startDateManual bit1=finishedDateManual
//   [17-18]   startDate.year            uint16_t LE
//   [19]      startDate.month           uint8_t
//   [20]      startDate.day             uint8_t
//   [21-22]   finishedDate.year         uint16_t LE
//   [23]      finishedDate.month        uint8_t
//   [24]      finishedDate.day          uint8_t
//   [25-40]   timeOfDaySeconds[4]       uint32_t LE each
//   [41-68]   dayOfWeekSeconds[7]       uint32_t LE each
//   [69-72]   estimatedTimeLeftSeconds  uint32_t LE, 0 means unavailable
//
// v6 (109 bytes) appends the reading-speed window (v5 fields unchanged):
//   [73-74]   wpm.avg                   uint16_t LE, trimmed mean WPM (0 = none)
//   [75-76]   wpm.count                 uint16_t LE, samples in window (0-15)
//   [77-106]  wpm.samples[15]           uint16_t LE each
//   [107]     wpm.pos                   uint8_t
//   [108]     reserved (0)              uint8_t
```
- **Bytes 12-15 are reserved in v6.** The legacy
  `{avgSecondsPerForwardPage, paceSampleCount}` pair is no longer written,
  read, or used anywhere. Reading speed is sourced exclusively from the WPM
  window at bytes 73-108. The bytes are kept as zeroed reservation slots to
  preserve the v5 field prefix offsets (no shift = same `decodeV6` byte
  arithmetic as before, simpler migration).
- The in-memory `BookReadingStats` struct no longer carries these fields.
- `resolveReadingPaceSecondsPerPage` no longer has a `paceSampleCount` /
  `avgSecondsPerForwardPage` fallback.

### 2.2 Kindle-style trimmed-mean algorithm
```cpp
// Constants (matching Kindle reverse-engineering):
static constexpr uint8_t kWpmWindowSize = 15;
static constexpr uint8_t kWpmTrimCount = 2;      // drop top-2 & bottom-2
static constexpr uint16_t kWpmHardCap = 900;     // discard if WPM > 900
static constexpr uint16_t kWpmFloor = 80;        // our lower bound

void recordForwardPageRead(uint32_t seconds, uint16_t wordsOnPage) {
    if (seconds == 0 || wordsOnPage == 0) return;
    uint32_t wpm = (wordsOnPage * 60u) / seconds;
    if (wpm > kWpmHardCap) return;           // hard discard
    if (wpm < kWpmFloor) wpm = kWpmFloor;    // floor
    
    // Circular buffer insert
    wpmWindow[wpmWindowPos] = (uint16_t)wpm;
    wpmWindowPos = (wpmWindowPos + 1) % kWpmWindowSize;
    if (wpmWindowCount < kWpmWindowSize) ++wpmWindowCount;
    
    // Compute trimmed mean
    avgWpm = computeTrimmedMean();
    wpmSampleCount = wpmWindowCount;
}

uint16_t computeTrimmedMean() {
    if (wpmWindowCount == 0) return 0;
    if (wpmWindowCount <= kWpmTrimCount * 2) return plainMean();
    
    // Copy + insertion sort (15 elements = 30 bytes stack)
    uint16_t sorted[15];
    memcpy(sorted, wpmWindow, wpmWindowCount * 2);
    insertionSort(sorted, wpmWindowCount);
    
    uint32_t sum = 0;
    for (uint8_t i = kWpmTrimCount; i < wpmWindowCount - kWpmTrimCount; ++i)
        sum += sorted[i];
    return (uint16_t)(sum / (wpmWindowCount - 2 * kWpmTrimCount));
}

// Clear per-book WPM only (keeps sessions, totals, dates, buckets)
void clearWpmStats() {
    avgWpm = 0;
    wpmSampleCount = 0;
    wpmWindowPos = 0;
    wpmWindowCount = 0;
    wpmWindow.fill(0);
}
```

### 2.3 GlobalReadingStats — parallel WPM window
```cpp
// v3 layout (159 B) -> v4 (195 B) appends the WPM window at bytes 159-194.
// v4 byte layout (195 bytes total, version = 4 at [0]):
//   [0]       version = 4
//   [1-4]     totalSessions             uint32_t LE
//   [5-8]     totalReadingSeconds       uint32_t LE
//   [9-12]    totalPagesTurned          uint32_t LE
//   [13-16]   completedBooks            uint32_t LE
//   [17-32]   timeOfDaySeconds[4]       uint32_t LE each
//   [33-60]   dayOfWeekSeconds[7]       uint32_t LE each
//   [61-64]   readingHistoryAnchorDay   uint32_t LE
//   [65-156]  readingHistoryBits[92]
//   [157-158] longestReadingStreak      uint16_t LE
//   -- end of v3 layout (159 bytes total) --
//   [159-160] wpm.avg                   uint16_t LE  (trimmed mean WPM)
//   [161-162] wpm.count                 uint16_t LE  (valid samples 0-15)
//   [163-192] wpm.samples[15]           uint16_t LE each
//   [193]     wpm.pos                   uint8_t       (next insertion slot)
//   [194]     reserved (0)              uint8_t
//   -- total 195 bytes --
```

The WPM window contributes 36 bytes (159..194 inclusive) which is what
the v3 -> v4 size bump accounts for. The previous text said "34 bytes"
which is wrong; 36 is the real number.
```

### 2.4 `resolveReadingPaceSecondsPerPage` update
```cpp
// Convert WPM → seconds-per-page for UI (using calibrated words-per-page)
static constexpr uint16_t kCalibratedWordsPerPage = 220;
std::optional<uint16_t> resolveReadingPaceSecondsPerPage(...) {
    if (book.wpmSampleCount < kWpmWindowSize) return std::nullopt;  // need full window
    return (kCalibratedWordsPerPage * 60) / book.avgWpm;
}
```

---

## 3. Backward Compatibility

**Explicit support matrix (per what this branch will load):**

| Record | Loaded? | Notes |
|--------|---------|-------|
| Book v6 (109 B, current) | ✅ | Written by this build |
| Book v5 (73 B) | ✅ → v6 in-place | Migrated on first save; legacy file deleted |
| Book v4 (69 B) | ❌ | Silently treated as missing — book starts fresh |
| Book v3 and earlier | ❌ | Same — book starts fresh |
| crossink unversioned `stats.bin` | ❌ | Same — book starts fresh |
| Global v4 (195 B, current) | ✅ | Written by this build |
| Global v3 (159 B) | ✅ → v4 in-place | Migrated on first save (overwrite in place) |
| Global v1 (13 B) / v2 (17 B) | ❌ | Treated as missing; global stats start fresh |

**What this means in practice:**

- A user on the previous develop (book v5, global v3) upgrades, reads a
  book, and the stats are silently upgraded to v6 / v4 on the first save.
  No data loss; the WPM window starts empty and fills as they read.
- A user on an even older build (book v4, global v1/v2) upgrades and
  starts with a fresh per-book stat record (sessions = 0). The global
  stats likewise start fresh. This is the **documented breakage** of
  this PR. v1/v2 and v4 are pre-release layouts that have not been
  shipped, and bringing the v4 → v5 and v1/v2 → v3 migrations into the
  loader would be dead code. We drop them; the loader is simpler.
- A v4 / crossink `stats.bin` file left on a user's SD is left untouched
  on disk (we do not delete it during the upgrade — `remove()` no longer
  knows about it). It is invisible to the loader, harmless, and can be
  cleaned up by the user with the "Delete book stats" action.
- **Reading speed source after the migration:** the WPM window is the sole
  reading-speed source in v6. The legacy `avgSecondsPerForwardPage` /
  `paceSampleCount` pair that v5 carried at bytes 12-15 is no longer
  written, read, or used anywhere. Bytes 12-15 are kept as reserved
  (zero) in v6 to preserve the v5 field-prefix offsets — no shift = same
  `decodeV6` byte arithmetic as before. The in-memory
  `BookReadingStats` struct no longer carries these fields.
- **Newer format (forward build):** the loader peeks the version byte
  for any file at least v3-sized and refuses to overwrite if the version
  is greater than `GLOBAL_STATS_VERSION` / `STATS_FILE_VERSION`. A torn
  write (truncated garbage file) still falls through to the backup.

---

## 3.1 Clear Speed Actions (UI)

### Per-Book Clear (in BookStatsActivity)
- **Location**: Existing "Clear reading speed" action row (added in median PR)
- **Action**: `BookReadingStats::clearWpmStats()` — zeros `avgWpm`, `wpmSampleCount`, `wpmWindow`, `wpmWindowPos`, `wpmWindowCount`
- **Preserves**: `sessionCount`, `totalReadingSeconds`, `totalPagesTurned`, `isCompleted`, `startDate`, `finishedDate`, time-of-day/day-of-week buckets, legacy median fields

### Global Clear (in GlobalStatsActivity)
- **Location**: New action row "Clear global reading speed" in GlobalStatsActivity list
- **Action**: `GlobalReadingStats::clearWpmStats()` — zeros `globalAvgWpm`, `globalWpmSampleCount`, `globalWpmWindow`, `globalWpmWindowPos`, `globalWpmWindowCount`
- **Preserves**: `globalSessionCount`, `globalTotalReadingSeconds`, `globalTotalPagesTurned`, all bucket stats

### i18n Keys
```yaml
STR_CLEAR_BOOK_PACE: "Clear reading speed"           # per-book (already added)
STR_CLEAR_GLOBAL_PACE: "Clear global reading speed"  # global (new)
```

---

## 4. Test Plan

| Test | Description |
|------|-------------|
| `ParagraphWordCount` | ASCII, Unicode, punctuation, empty, CJK |
| `SectionPaginationWords` | Known paragraph counts → verify `wordsOnPage` |
| `WpmHardCapDiscard` | 901 WPM sample discarded |
| `WpmTrimmedMean` | 15 samples, drop top-2/bottom-2, verify mean |
| `WpmFloor` | Sub-80 WPM clamped to 80 |
| `ResolvePaceFromWpm` | 220 WPM → ~27 sec/page; 300 WPM → ~20 sec/page |
| `BackwardCompatV5` | Load v5 stats → WPM fields zero → falls back to median |

---

## 5. RAM/Flash Budget (ESP32-C3)

| Component | Cost |
|-----------|------|
| `wpmWindow[15]` | 30 bytes |
| Sort scratch `uint16_t[15]` | 30 bytes (stack, temporary) |
| Code size | ~2 KB flash |
| Total | **< 100 bytes RAM, ~2 KB flash** |

---

## 6. Open Questions

1. **v6 binary format**: Break old stats (clean) or dual-write both algorithms during transition?
2. **Global WPM**: Separate `GlobalReadingStats` window or share book window after N words?
3. **Calibrated words-per-page**: 220 is Kindle's assumption. Should it be configurable per font/layout?
4. **CJK word counting**: `Paragraph::wordCount()` needs ICU or simple heuristic for non-space languages.

---

## 7. Implementation Order

1. `Paragraph::wordCount()` + unit test
2. `Section` pagination accumulates `wordsOnPage` → `onPageComplete` signature change
3. `EpubReaderActivity` threads `wordsOnPage` to `recordForwardPageRead(seconds, words)`
4. `BookReadingStats` v6 + Kindle WPM tracker + `clearWpmStats()` + drop legacy `avgSecondsPerForwardPage`/`paceSampleCount`
5. `GlobalReadingStats` parallel WPM window + `clearWpmStats()` (no legacy field to clear)
6. `ReadingStatsUtils` updated `resolveReadingPaceSecondsPerPage` (legacy fallback removed)
7. `BookStatsActivity` row reuses WPM value (renamed "Reading Speed")
8. Drop very-old format fallbacks (v1/v2 global, v4 book, crossink unversioned `stats.bin`)
9. v5 → v6 in-place migration on save
10. Tests + `pio run -e x4pro` build verification

**Status as of this revision:** items 1-10 are implemented on this branch
(see commits `d9e3b9e4`, `b23671a6`, `228706fc`, and follow-up). The EPUB
pipeline plumbing in `EpubReaderActivity` calls `p->wordCount()` (a
member on the `ParsedText`/`Page` value) on every page load and threads
the count into `recordForwardPagePaceSample`, which feeds both
`BookReadingStats::recordForwardPageRead` and
`GlobalReadingStats::recordGlobalPageRead`. The Reading-Speed row in
`BookStatsActivity` displays the WPM value from the per-book window
once it has at least one sample.

**This design doc is now a historical record of the WPM plumbing and the
v5 -> v6 (and v3 -> v4 global) migration. The implementation is in this
branch and the PR's CI is green. There is no remaining "next step" to
implement from this design.**
