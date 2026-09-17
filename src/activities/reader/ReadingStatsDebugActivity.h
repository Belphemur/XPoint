#pragma once
// Dev-only diagnostics page: dumps the raw persisted WPM / session windows so
// a stuck reading-speed value can be compared against the stored samples on
// device. Shows the global windows plus one compact line per book that has
// stats data (enumerated from the library index); selecting a book opens its
// full raw dump. Compiles out of everything but serial-debug builds with
// reading stats enabled.
#if defined(READING_STATS_ENABLED) && defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"

// Static text dump of the rolling windows behind the reading-speed card.
class ReadingStatsDebugActivity final : public Activity {
 public:
  explicit ReadingStatsDebugActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Full record kept resident: enumerateBooks() loads each book's stats once
  // from SD and render()/renderBookDump() only format already-resident data
  // (render() runs once per display refresh; re-reading the 5-candidate
  // stats-file probe chain per frame stalls the UI on device hardware).
  struct BookLine {
    std::string title;
    std::string cachePath;
    BookReadingStats stats;
  };

  void enumerateBooks();
  // Renders one window: an avg/cnt/pos line followed by the sample array in
  // raw storage order, 8 samples per line. y is the shared cursor advanced
  // per drawn line.
  void dumpSamples(const char* label, const uint16_t* samples, size_t windowSize, size_t count, size_t pos,
                   uint16_t avg, int& y) const;
  void renderList(int lineH) const;
  void renderBookDump(int lineH) const;

  GlobalReadingStats globalStats;
  std::vector<BookLine> books;
  int cursor = 0;  // selected book line (list level)
  // Scroll clamp runs inside const render(); sequenced with loop() through
  // requestUpdate, so no locking beyond what cursor already relies on.
  mutable int offset = 0;    // first visible book line (list level)
  bool showingBook = false;  // true: full dump of books[detailIndex]
  int detailIndex = 0;
  GfxRenderer::Orientation previousOrientation = GfxRenderer::Orientation::Portrait;
};

#endif  // dev-build gate
