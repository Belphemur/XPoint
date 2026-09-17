#include "ReadingStatsDebugActivity.h"

#if defined(READING_STATS_ENABLED) && defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2

#include <GfxRenderer.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "BookCachePath.h"
#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
// Samples are grouped 8 per line so a full WPM window (15) takes two lines.
constexpr size_t kSamplesPerLine = 8;
constexpr size_t kTitleChars = 38;     // full-dump title line
constexpr size_t kRowTitleChars = 22;  // compact list row
constexpr int kLeftX = 4;

// One sample line: "  s[08-14] 80 80 104<7 ...". When the window is full the
// sample at the wrap slot pos is tagged with "<pos".
void formatSampleLine(char* buf, size_t len, size_t first, size_t last, const uint16_t* samples, size_t count,
                      size_t pos, bool full) {
  size_t used = static_cast<size_t>(snprintf(buf, len, "  s[%02zu-%02zu]", first, last));
  if (used >= len) return;
  for (size_t i = first; i <= last && i < count; ++i) {
    int written;
    if (full && i == pos) {
      written =
          snprintf(buf + used, len - used, " %u<%u", static_cast<unsigned>(samples[i]), static_cast<unsigned>(pos));
    } else {
      written = snprintf(buf + used, len - used, " %u", static_cast<unsigned>(samples[i]));
    }
    if (written < 0) return;
    used += static_cast<size_t>(written);
    if (used >= len) return;
  }
}
}  // namespace

// Draws one dump line and mirrors the FULL text to the serial DBG log: the
// screen can only show what fits the panel width, the log always shows
// everything (device log 2026-09-17: long header/sample lines ran past the
// 480px panel and flooded [ERR][GFX] Outside-range spam after rotation).
void ReadingStatsDebugActivity::drawDumpLine(const char* text, const int y) const {
  LOG_DBG("RSDBG", "%s", text);
  const int maxY = renderer.getScreenWidth() - kLeftX - 2;
  if (renderer.getTextWidth(SMALL_FONT_ID, text) <= maxY) {
    renderer.drawText(SMALL_FONT_ID, kLeftX, y, text);
    return;
  }
  // Byte-wise truncate until it fits (dev page, ASCII labels; a split
  // multi-byte tail is dropped by the renderer).
  char clipped[128];
  snprintf(clipped, sizeof(clipped), "%s", text);
  size_t len = strlen(clipped);
  while (len > 1 && renderer.getTextWidth(SMALL_FONT_ID, clipped) > maxY) {
    clipped[--len] = '\0';
  }
  renderer.drawText(SMALL_FONT_ID, kLeftX, y, clipped);
}

ReadingStatsDebugActivity::ReadingStatsDebugActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ReadingStatsDebug", renderer, mappedInput) {}

void ReadingStatsDebugActivity::onEnter() {
  Activity::onEnter();
  // Match the other reading-stats screens: the dump is laid out for portrait.
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  globalStats = GlobalReadingStats::load();
  enumerateBooks();
  requestUpdate();
}

void ReadingStatsDebugActivity::onExit() {
  renderer.setOrientation(previousOrientation);
  Activity::onExit();
}

void ReadingStatsDebugActivity::enumerateBooks() {
  books.clear();
  library::LibraryIndexFile index;
  if (!index.open(library::libraryIndexPath())) {
    LOG_ERR("RSDBG", "library index unavailable");
    return;
  }
  const auto count = index.bookCount();
  // One compact entry per book with window data; a few hundred bytes each,
  // bounded by the library size. Dev-only page, accepted.
  books.reserve(std::min<size_t>(count, 64));
  for (uint16_t ordinal = 0; ordinal < count; ++ordinal) {
    library::ClixRecord record;
    if (!index.readRecord(ordinal, record)) {
      LOG_ERR("RSDBG", "record %u unreadable", static_cast<unsigned>(ordinal));
      break;
    }
    std::string path;
    if (!index.readPath(record, path)) {
      continue;
    }
    const std::string cachePath = cachePathForBookPath(path);
    if (cachePath.empty()) {
      continue;
    }
    // No stats file loads as a default record: wpm/session counts of 0.
    const BookReadingStats stats = BookReadingStats::load(cachePath);
    if (stats.wpm.count == 0 && stats.sessionWindow.count == 0) {
      continue;
    }
    std::string title;
    index.readTitle(record, title);
    BookLine line;
    line.title = !title.empty() ? std::move(title) : std::move(path);
    line.cachePath = cachePath;
    line.stats = stats;
    books.push_back(std::move(line));
  }
}

void ReadingStatsDebugActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (showingBook) {
      showingBook = false;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }
  if (showingBook) {
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (cursor > 0) {
      --cursor;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (cursor + 1 < static_cast<int>(books.size())) {
      ++cursor;
      requestUpdate();
    }
    return;
  }
  if ((mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
       mappedInput.wasReleased(MappedInputManager::Button::Right)) &&
      !books.empty()) {
    detailIndex = cursor;
    showingBook = true;
    requestUpdate();
  }
}

void ReadingStatsDebugActivity::dumpSamples(const char* label, const uint16_t* samples, const size_t windowSize,
                                            const size_t count, const size_t pos, const uint16_t avg, int& y) const {
  char buf[128];
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  auto nextY = [&y, lineH]() {
    y = y < 0 ? 2 : y + lineH;
    return y;
  };

  if (count == 0) {
    snprintf(buf, sizeof(buf), "%s avg=%u cnt=0 (empty)", label, avg);
    drawDumpLine(buf, nextY());
    return;
  }

  const bool full = count >= windowSize;
  snprintf(buf, sizeof(buf), "%s avg=%u cnt=%u pos=%u", label, avg, static_cast<unsigned>(count),
           static_cast<unsigned>(pos));
  drawDumpLine(buf, nextY());
  for (size_t first = 0; first < count; first += kSamplesPerLine) {
    const size_t last = first + kSamplesPerLine - 1 < windowSize ? first + kSamplesPerLine - 1 : windowSize - 1;
    formatSampleLine(buf, sizeof(buf), first, last, samples, count, pos, full);
    drawDumpLine(buf, nextY());
  }
}

void ReadingStatsDebugActivity::renderList(const int lineH) const {
  char buf[128];
  int y = -1;
  auto nextY = [&y, lineH]() {
    y = y < 0 ? 2 : y + lineH;
    return y;
  };

  snprintf(buf, sizeof(buf), "RS DEBUG  wpm w=%u trim=%u floor=%u cap=%u", static_cast<unsigned>(WPM_WINDOW_SIZE),
           static_cast<unsigned>(WPM_TRIM_COUNT), WPM_FLOOR, WPM_HARD_CAP);
  drawDumpLine(buf, nextY());
  snprintf(buf, sizeof(buf), "sess w=%u trim=%u min=%us", static_cast<unsigned>(SESSION_WINDOW_SIZE),
           static_cast<unsigned>(SESSION_TRIM_COUNT), static_cast<unsigned>(SESSION_MIN_SECONDS));
  drawDumpLine(buf, nextY());

  dumpSamples("GLOB wpm", globalStats.wpm.samples.data(), WPM_WINDOW_SIZE, globalStats.wpm.count, globalStats.wpm.pos,
              globalStats.wpm.avg, y);
  dumpSamples("GLOB sess", globalStats.sessionWindow.samples.data(), SESSION_WINDOW_SIZE,
              globalStats.sessionWindow.count, globalStats.sessionWindow.pos, globalStats.sessionWindow.avg, y);
  snprintf(buf, sizeof(buf), "GLOB turns=%u sess=%u", static_cast<unsigned>(globalStats.totalPagesTurned),
           static_cast<unsigned>(globalStats.totalSessions));
  drawDumpLine(buf, nextY());

  // Compact per-book lines below the global dump, scrolled to keep the cursor
  // visible. Up/Down move the cursor, Confirm opens the full dump.
  if (books.empty()) {
    drawDumpLine("no books with stats data", nextY());
    return;
  }

  const int rowsTop = y + lineH / 2;
  const int rowStride = lineH + 2;
  int visibleRows = (renderer.getScreenHeight() - rowsTop - 4) / rowStride;
  visibleRows = std::max(1, visibleRows);
  if (cursor < offset) {
    offset = cursor;
  }
  if (cursor >= offset + visibleRows) {
    offset = cursor - visibleRows + 1;
  }

  for (int row = 0; row < visibleRows && offset + row < static_cast<int>(books.size()); ++row) {
    const BookLine& book = books[offset + row];
    snprintf(buf, sizeof(buf), "%s%.22s w=%u/%u s=%u/%u", offset + row == cursor ? ">" : " ", book.title.c_str(),
             book.stats.wpm.avg, book.stats.wpm.count, book.stats.sessionWindow.avg, book.stats.sessionWindow.count);
    drawDumpLine(buf, rowsTop + row * rowStride);
  }
}

void ReadingStatsDebugActivity::renderBookDump(const int lineH) const {
  const BookLine& book = books[detailIndex];
  char buf[128];
  int y = -1;
  auto nextY = [&y, lineH]() {
    y = y < 0 ? 2 : y + lineH;
    return y;
  };

  snprintf(buf, sizeof(buf), "RS DEBUG  wpm w=%u trim=%u floor=%u cap=%u", static_cast<unsigned>(WPM_WINDOW_SIZE),
           static_cast<unsigned>(WPM_TRIM_COUNT), WPM_FLOOR, WPM_HARD_CAP);
  drawDumpLine(buf, nextY());
  snprintf(buf, sizeof(buf), "sess w=%u trim=%u min=%us", static_cast<unsigned>(SESSION_WINDOW_SIZE),
           static_cast<unsigned>(SESSION_TRIM_COUNT), static_cast<unsigned>(SESSION_MIN_SECONDS));
  drawDumpLine(buf, nextY());

  // Dev-only page, plain ASCII labels; a byte-wise truncation of the title is
  // good enough (invalid mid-codepoint tails are dropped by the renderer).
  if (book.title.size() > kTitleChars) {
    snprintf(buf, sizeof(buf), "BOOK: %.38s...", book.title.c_str());
  } else {
    snprintf(buf, sizeof(buf), "BOOK: %s", book.title.c_str());
  }
  drawDumpLine(buf, nextY());
  // Cache dir basename identifies the stats file being dumped.
  const size_t slash = book.cachePath.find_last_of('/');
  snprintf(buf, sizeof(buf), "dir: %s",
           slash == std::string::npos ? book.cachePath.c_str() : book.cachePath.c_str() + slash + 1);
  drawDumpLine(buf, nextY());

  // Loaded once in enumerateBooks() and kept resident in BookLine; render()
  // must never touch SD (it can run once per display refresh).
  const BookReadingStats& stats = book.stats;
  dumpSamples("BOOK wpm", stats.wpm.samples.data(), WPM_WINDOW_SIZE, stats.wpm.count, stats.wpm.pos, stats.wpm.avg, y);
  dumpSamples("BOOK sess", stats.sessionWindow.samples.data(), SESSION_WINDOW_SIZE, stats.sessionWindow.count,
              stats.sessionWindow.pos, stats.sessionWindow.avg, y);
  if (stats.lastBookProgressPercent == UNKNOWN_BOOK_PROGRESS_PERCENT) {
    snprintf(buf, sizeof(buf), "BOOK turns=%u progress=-", static_cast<unsigned>(stats.totalPagesTurned));
  } else {
    snprintf(buf, sizeof(buf), "BOOK turns=%u progress=%u%%", static_cast<unsigned>(stats.totalPagesTurned),
             static_cast<unsigned>(stats.lastBookProgressPercent));
  }
  drawDumpLine(buf, nextY());
}

void ReadingStatsDebugActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  if (showingBook && detailIndex >= 0 && detailIndex < static_cast<int>(books.size())) {
    renderBookDump(lineH);
  } else {
    renderList(lineH);
  }
  renderer.displayBuffer();
}

#endif  // dev-build gate
