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

### 2.1 Binary format version bump (v5 → v6)
```cpp
// New fields in BookReadingStats (persisted to SD):
uint16_t avgWpm;              // trimmed-mean WPM (0 = not enough samples)
uint16_t wpmSampleCount;      // valid samples in window (0–15)
std::array<uint16_t, 15> wpmWindow{};  // rolling WPM samples
uint8_t  wpmWindowPos = 0;
uint8_t  wpmWindowCount = 0;
```
- Keep existing `avgSecondsPerForwardPage` + `paceSampleCount` for backward compatibility during transition
- Or repurpose them (v6 breaks old stats anyway — acceptable per design)

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
// New fields in GlobalReadingStats (persisted to SD, v6):
uint16_t globalAvgWpm = 0;            // cross-book trimmed-mean WPM
uint16_t globalWpmSampleCount = 0;    // valid samples (0–15)
std::array<uint16_t, 15> globalWpmWindow{};  // rolling WPM samples
uint8_t  globalWpmWindowPos = 0;
uint8_t  globalWpmWindowCount = 0;

// Called from EpubReaderActivity after book's recordForwardPageRead
void recordGlobalPageRead(uint32_t seconds, uint16_t wordsOnPage) {
    // Same trimmed-mean logic as BookReadingStats::recordForwardPageRead
    // but writes to globalWpmWindow / globalAvgWpm / globalWpmSampleCount
}

void clearWpmStats() {
    globalAvgWpm = 0;
    globalWpmSampleCount = 0;
    globalWpmWindowPos = 0;
    globalWpmWindowCount = 0;
    globalWpmWindow.fill(0);
}
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

- **Binary format v6**: New fields appended. Old v5 files load with `avgWpm=0`, `wpmSampleCount=0` → falls back to legacy seconds-per-page median until 15 new samples collected.
- **GlobalReadingStats**: Add parallel `globalAvgWpm` / `globalWpmWindow` for cross-book estimate.
- **Settings**: No new settings needed (trim count, cap, window size are constants).

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
4. `BookReadingStats` v6 + Kindle WPM tracker + `clearWpmStats()`
5. `GlobalReadingStats` parallel WPM window
6. `ReadingStatsUtils` updated `resolveReadingPaceSecondsPerPage`
7. Tests + `pio run -e x4pro` build verification

---

**Ready for implementation.**
