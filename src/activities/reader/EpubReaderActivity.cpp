#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <esp_system.h>
#include <freertos/task.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <variant>

#include "../../util/BookmarkFile.h"
#include "BoardFeatures.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "MemSentinel.h"
#if defined(CROSSPOINT_TTF_READER)
#include "TtfWordSelect.h"
#endif
#include "activities/ActivityResult.h"
#include "activities/util/IntervalSelectionActivity.h"
#ifdef READING_STATS_ENABLED
#include "BookStatsActivity.h"
#include "FinishedBooksActivity.h"
#include "FinishedBooksIndex.h"
#include "GlobalReadingStats.h"
#include "ReadingRhythmActivity.h"
#include "ReadingStatsMenuActivity.h"
#include "activities/settings/GlobalStatsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#endif
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderFontSizes.h"
#include "ReaderToolbarUi.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/settings/TextSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

#if defined(CROSSPOINT_TTF_READER)
#include <BookFontLoader.h>
#include <render/PageRenderer.h>

#include "activities/reader/ProgressRecord.h"
#include "adapters/FrameTargetFactory.h"
#include "adapters/PagePaint.h"
#endif

namespace {
// The X4 Pro and X4 Classic carry the X4's panel but sit outside isXteinkDevice()
// (that helper also gates power management). Overlay refresh choices are per-panel:
// this family runs the grayscale anti-aliasing pass, so chrome painted over a
// fresh page needs the HALF ghost-cleanup and closing re-renders the page.
bool xteinkClassPanel() { return gpio.isXteinkDevice() || BoardConfig::isX4Pro() || BoardConfig::isX4Classic(); }

constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
// Cadence for re-rendering while a chapter build is delegated to the FIBP
// prefetch worker (worker owns the build; the reader just polls for commit).
constexpr unsigned long fibpDeferPollMs = 250;
// Grace before the reader takes a delegated build over from the worker: two
// page builds at full speed. Past it the user would otherwise sit on the
// Indexing popup for the rest of the spine (a 93-page spine measured 243 s
// at low power, tens of seconds even at full speed).
constexpr unsigned long fibpDeferTakeoverMs = 1500;
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

#ifdef READING_STATS_ENABLED
// Minimum time a page must be shown before its dwell time counts toward
// reading statistics (matches CrossInk's MIN_READING_STATS_PAGE_MS).
constexpr unsigned long MIN_READING_STATS_PAGE_MS = 2000UL;
// Minimum forward-page dwell (seconds) before it is recorded as a pace sample.
constexpr uint32_t MIN_READING_PACE_SAMPLE_SECONDS = 2;
// Pages shown longer than this are treated as idle and excluded from stats.
constexpr uint32_t READING_IDLE_THRESHOLD_SECONDS = 600;
#endif

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// Unrounded book-progress percent (0..100) for the page currently shown, used
// by both the reader menu and the stats exit snapshot so both report the same
// convention: chapterProgress = currentPage / estimatedTotalPages.
float computeBookProgressPercent(const Epub& epub, const Section* section, const int spineIndex) {
  if (epub.getBookSize() == 0 || !section || section->estimatedTotalPages() == 0) {
    return 0.0f;
  }
  const float chapterProgress =
      static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
  return epub.calculateProgress(spineIndex, chapterProgress) * 100.0f;
}

constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    // No stats handling needed here: per-book stats live INSIDE the cache dir,
    // so renaming the dir moves the stats file with the book.
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
#ifdef READING_STATS_ENABLED
  FinishedBooksIndex::migratePath(srcPath, dstPath);
#endif
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

EpubReaderActivity::~EpubReaderActivity() {
  ImageBlock::setExtractor(nullptr, nullptr);
#ifdef BOARD_HAS_PSRAM
  ImageBlock::setPsramExtractor(nullptr, nullptr);
#endif
  if (overlayRefreshPending.load()) {
    RenderLock lock;  // whatever screen follows paints the framebuffer
    settleOverlayRefresh();
  }
  discardOverlayPage();  // free the overlay's page snapshot if one is held

  // Design §4.4: exit flushing is the manager's job — closeBook() flushes
  // any unflushed change synchronously (bounded by one record write) and
  // resets the manager state. The manager stays valid after this; no
  // background tick can touch reader state.
  progressManager.closeBook();

  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
#if defined(CROSSPOINT_TTF_READER)
    if (ttf_) {
      // Page-anchored TTF record (charOffset 0 + generation): the restore
      // maps the record's page number (§3.5 item 8). Persist the origin
      // chapter's page count when a COMPLETE source can answer it, so the
      // load-side corrupt-page guard applies; 0 keeps the deferred
      // within-chapter clamp as the bound for unbuilt chapters.
      const bool sessionComplete =
          ttf_->sessionFor(origin.spineIndex) && ttf_->sessionDone() && ttf_->sessionMatchesGeneration(ttfGeneration);
      // Same completeness contract as the restore path (ttfResolveTargetPage):
      // the cache answers only its own chapter at the current generation, and
      // never while a session for that spine is mid-build (partial writer
      // count must not masquerade as the chapter total).
      const bool cacheComplete = !ttf_->sessionFor(origin.spineIndex) && ttf_->cacheReady() && !ttf_->cachePartial() &&
                                 ttf_->cacheSpine() == origin.spineIndex && ttf_->cacheGeneration() == ttfGeneration;
      const uint16_t originPageCount =
          (sessionComplete || cacheComplete) ? static_cast<uint16_t>(ttf_->availablePageCount(origin.spineIndex)) : 0;
      if (!progressManager.saveNowTtf(epub->getCachePath().c_str(), origin.spineIndex, origin.pageNumber,
                                      originPageCount, 0, ttfGeneration)) {
        LOG_ERR("ERS", "TTF footnote-origin progress save failed");
      }
    } else {
#endif
      std::optional<uint32_t> offset;
      if (section && origin.spineIndex == currentSpineIndex && origin.pageNumber >= 0 &&
          origin.pageNumber < section->pageCount) {
        offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(origin.pageNumber));
      }
      // Single-writer rule (design §4.7): the footnote-origin save routes
      // through the saver like every other synchronous save.
      progressManager.saveNow(epub->getCachePath().c_str(), origin.spineIndex, origin.pageNumber, 0, offset.has_value(),
                              offset.value_or(0));
#if defined(CROSSPOINT_TTF_READER)
    }
#endif
  }

  section.reset();
  nextSectionPrefetch.reset();
  nextSectionSpineIndex = -1;
#if defined(CROSSPOINT_TTF_READER)
  // Abort/suspend any open session (partial cache commit) and release the
  // arenas BEFORE the progress manager's state is gone but while epub is
  // still valid for logging.
  ttf_.reset();
#endif
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::onEnter() {
  ReaderActivity::onEnter();
#if defined(CROSSPOINT_TTF_READER)
  // Sync the FT faces' render mode with the persisted setting before any
  // render/worker session can use them (a Crisp mode saved in a previous
  // session must apply at boot, not only after a settings visit).
  if (ttf_) {
    freeink::book::fontLoader.applyRenderMode(SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_CRISP);
  }
#endif
#ifdef READING_STATS_ENABLED
  if (epub && SETTINGS.shouldTrackReadingStats()) {
    stats = BookReadingStats::load(epub->getCachePath());
    globalStats = GlobalReadingStats::load();
    sessionReadingSeconds = 0;
    sessionPageTurns = 0;
    hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);
  }
#endif
}

void EpubReaderActivity::onExit() {
#if defined(CROSSPOINT_TTF_READER)
  // R1: transient worker lifetime — cancel + join + free before teardown.
  stopFibpWorker();
#endif
#ifdef READING_STATS_ENABLED
  if (epub && SETTINGS.shouldTrackReadingStats()) {
    recordCurrentPageReadingTime();
    const uint32_t elapsedSecs = sessionReadingSeconds;
    if (elapsedSecs >= 60) {
      stats.sessionCount++;
      globalStats.totalSessions++;
    }
    if (elapsedSecs >= 10) {
      stats.totalReadingSeconds += elapsedSecs;
      globalStats.totalReadingSeconds += elapsedSecs;
      if (hasSessionStartLocalDateTime) {
        stats.recordReadingSpan(sessionStartLocalDateTime, elapsedSecs);
        globalStats.recordReadingSpan(sessionStartLocalDateTime, elapsedSecs);
      }
      if (elapsedSecs >= 120 && !stats.startDateManual && !stats.startDate.isValid() && hasSessionStartLocalDateTime) {
        stats.startDate = sessionStartLocalDateTime.date;
      }
    }
    if (elapsedSecs >= SESSION_MIN_SECONDS && sessionPageTurns >= SESSION_MIN_PAGE_TURNS) {
      // The Avg Session window has its own gates (lower time threshold, plus
      // an engagement check): it accepts sessions the Sessions counter
      // ignores, and rejects "book left open" sessions that turn no pages.
      stats.recordSession(elapsedSecs);
      globalStats.recordGlobalSession(elapsedSecs);
    }
    if (epub) {
      uint16_t chapterPages = section ? section->estimatedTotalPages() : 0;
#if defined(CROSSPOINT_TTF_READER)
      if (!section && ttf_) chapterPages = static_cast<uint16_t>(ttfPageCount);
#endif
      const uint64_t bookSize = epub->getBookSize();
      const uint64_t chapterEnd = epub->getCumulativeSpineItemSize(currentSpineIndex);
      const uint64_t chapterStart =
          currentSpineIndex >= 1 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
      const uint64_t chapterBytes = (chapterEnd > chapterStart) ? (chapterEnd - chapterStart) : 0;
#if defined(CROSSPOINT_TTF_READER)
      float bookProgressPercent;
      if (ttf_ && !section) {
        // Mirrors-based percent (computeBookProgressPercent needs a Section).
        const float chapterProgress =
            cachedChapterTotalPageCount > 0
                ? static_cast<float>(ttfPage) / static_cast<float>(cachedChapterTotalPageCount)
                : 0.0f;
        bookProgressPercent = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      } else {
        bookProgressPercent = computeBookProgressPercent(*epub, section.get(), currentSpineIndex);
      }
#else
      const float bookProgressPercent = computeBookProgressPercent(*epub, section.get(), currentSpineIndex);
#endif
      if (bookSize > 0 && chapterBytes > 0 && chapterPages > 0 && bookProgressPercent >= 0.0f &&
          bookProgressPercent <= 100.0f) {
        const uint64_t bookPagesEstimate = static_cast<uint64_t>(chapterPages) * bookSize / chapterBytes;
        const float bookProgress = bookProgressPercent / 100.0f;
        const uint32_t remainingPages =
            static_cast<uint32_t>((1.0f - bookProgress) * static_cast<float>(bookPagesEstimate));
        auto timeLeft = estimateBookTimeLeftSeconds(stats, globalStats, remainingPages, currentPageWordsOnPage);
        if (timeLeft) {
          stats.estimatedTimeLeftSeconds = *timeLeft;
        }
        stats.lastBookProgressPercent =
            static_cast<uint8_t>(clampPercent(static_cast<int>(bookProgressPercent + 0.5f)));
      }
    }
    // Two independent sequential-record writes (per-book + global). The global
    // save is atomic (tmp -> verify -> .bak rotate -> rename) on its own, so a
    // failure between them degrades gracefully: the book record may be ahead
    // of the aggregate until the next session commit.
    stats.save(epub->getCachePath());
    globalStats.save();
    if (stats.isCompleted) syncFinishedBookIndex();
  }
#endif
  ReaderActivity::onExit();
}

#ifdef READING_STATS_ENABLED
bool EpubReaderActivity::currentPageReadingSecondsForStats(uint32_t& seconds) const {
  if (!SETTINGS.shouldTrackReadingStats() || pageShownAtMs == 0) return false;
  const unsigned long elapsedMs = millis() - pageShownAtMs;
  if (elapsedMs < MIN_READING_STATS_PAGE_MS) return false;
  const unsigned long elapsedSeconds = elapsedMs / 1000;
  if (elapsedSeconds == 0 || elapsedSeconds > READING_IDLE_THRESHOLD_SECONDS) return false;
  seconds = static_cast<uint32_t>(elapsedSeconds);
  return true;
}

void EpubReaderActivity::recordCurrentPageReadingTime() {
  uint32_t seconds = 0;
  if (!currentPageReadingSecondsForStats(seconds)) {
    pageShownAtMs = 0;
    return;
  }
  if (UINT32_MAX - sessionReadingSeconds < seconds) {
    sessionReadingSeconds = UINT32_MAX;
  } else {
    sessionReadingSeconds += seconds;
  }
  pageShownAtMs = 0;
}

void EpubReaderActivity::recordForwardPagePaceSample(uint32_t seconds, uint16_t wordsOnPage) {
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  // Raw inputs of every pace sample (accepted or rejected): the WpmWindow
  // stores the floor-clamped value, so a stuck-at-80 device dump cannot show
  // whether words were undercounted or dwell inflated — this line can.
  const uint32_t rawWpm = (seconds > 0 && wordsOnPage > 0) ? (static_cast<uint32_t>(wordsOnPage) * 60U) / seconds : 0;
  LOG_DBG("RSDBG", "sample dwell=%us words=%u rawWpm=%lu", static_cast<unsigned>(seconds),
          static_cast<unsigned>(wordsOnPage), static_cast<unsigned long>(rawWpm));
#endif
  if (seconds < MIN_READING_PACE_SAMPLE_SECONDS) return;
  stats.recordForwardPageRead(seconds, wordsOnPage);
  globalStats.recordGlobalPageRead(seconds, wordsOnPage);
}

void EpubReaderActivity::applyBookStatsEditsFromDisk() {
  if (!epub || !SETTINGS.shouldTrackReadingStats()) return;

  // Only editable completion fields come back from the stats screen. Its copy
  // contains preview counters, so importing totals/sessions here would
  // double-count the in-flight reading session.
  const BookReadingStats diskStats = BookReadingStats::load(epub->getCachePath());
  stats.isCompleted = diskStats.isCompleted;
  stats.startDateManual = diskStats.startDateManual;
  stats.finishedDateManual = diskStats.finishedDateManual;
  stats.completionAchievementPending = diskStats.completionAchievementPending;
  stats.completionPromptDismissedAtHundred = diskStats.completionPromptDismissedAtHundred;
  stats.startDate = diskStats.startDate;
  stats.finishedDate = diskStats.finishedDate;

  globalStats.completedBooks = GlobalReadingStats::load().completedBooks;
}

void EpubReaderActivity::syncFinishedBookIndex() {
  // An entry without a title can never be loaded back (loadPath() skips it),
  // and would break the whole index rewrite's verify pass.
  if (epub && epub->getTitle().empty()) {
    LOG_ERR("ERS", "Skipping finished-book entry: empty title");
    return;
  }
  if (!epub || !FinishedBooksIndex::recordCanonical(epub->getPath(), epub->getCachePath(), epub->getTitle(),
                                                    epub->getAuthor(), stats)) {
    LOG_ERR("ERS", "Failed to synchronize finished-book entry");
  }
}

void EpubReaderActivity::handleBookStatsReturn() {
  const bool wasCompleted = stats.isCompleted;
  applyBookStatsEditsFromDisk();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (!wasCompleted && stats.isCompleted) {
      RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (wasCompleted && !stats.isCompleted) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
    }
  }
  if (!wasCompleted && stats.isCompleted && SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath())) {
    pendingReadFolderMove = true;
  } else if (!stats.isCompleted) {
    pendingReadFolderMove = false;
  }
  syncFinishedBookIndex();
}

BookReadingStats EpubReaderActivity::achievementStatsPreview(uint32_t* pendingReadingSeconds) const {
  if (pendingReadingSeconds) *pendingReadingSeconds = 0;
  BookReadingStats preview = stats;
  if (!SETTINGS.shouldTrackReadingStats()) return preview;

  uint32_t pendingSeconds = sessionReadingSeconds;
  uint32_t currentPageSeconds = 0;
  if (currentPageReadingSecondsForStats(currentPageSeconds)) {
    pendingSeconds =
        pendingSeconds > UINT32_MAX - currentPageSeconds ? UINT32_MAX : pendingSeconds + currentPageSeconds;
  }
  if (pendingReadingSeconds) *pendingReadingSeconds = pendingSeconds;

  preview.totalReadingSeconds = preview.totalReadingSeconds > UINT32_MAX - pendingSeconds
                                    ? UINT32_MAX
                                    : preview.totalReadingSeconds + pendingSeconds;
  if (pendingSeconds >= 60 && preview.sessionCount < UINT16_MAX) ++preview.sessionCount;
  if (pendingSeconds >= 10 && hasSessionStartLocalDateTime) {
    preview.recordReadingSpan(sessionStartLocalDateTime, pendingSeconds);
  }
  return preview;
}

void EpubReaderActivity::goHomeOrShowCompletionAchievement() {
  const int spineCount = epub ? epub->getSpineItemsCount() : 0;
  const bool atEndOfBook = epub && spineCount > 0 && currentSpineIndex >= spineCount;
  const bool onFinalPage = epub && section && spineCount > 0 && currentSpineIndex == spineCount - 1 &&
                           section->pageCount > 0 && section->currentPage >= section->pageCount - 1;
  const bool displaysHundredPercent =
      epub && computeBookProgressPercent(*epub, section.get(), currentSpineIndex) >= 99.5f;

  // Crossing past the final readable page is definitive completion evidence.
  // A final page or rounded 100% can also come from a jump, so ask first.
  if (!stats.isCompleted && atEndOfBook) {
    setBookCompleted(true);
  } else if (!stats.isCompleted && !stats.completionPromptDismissedAtHundred &&
             (onFinalPage || displaysHundredPercent)) {
    auto prompt = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_MARK_FINISHED_PROMPT), "");
    if (!prompt) {
      LOG_ERR("ERS", "OOM: completion prompt");
      // Without a prompt the reader asked to leave, not to complete: do not
      // silently record completion state they never confirmed.
      onGoHome();
      return;
    }
    startActivityForResult(std::move(prompt), [this](const ActivityResult& result) {
      if (result.isCancelled) {
        stats.completionPromptDismissedAtHundred = true;
        if (epub) stats.save(epub->getCachePath());
        onGoHome();
        return;
      }
      setBookCompleted(true);
      goHomeOrShowCompletionAchievement();
    });
    return;
  }

  if (!stats.isCompleted || !stats.completionAchievementPending || !epub) {
    onGoHome();
    return;
  }

  uint32_t pendingReadingSeconds = 0;
  const BookReadingStats achievementStats = achievementStatsPreview(&pendingReadingSeconds);
  GlobalReadingStats achievementGlobalStats = globalStats;
  if (pendingReadingSeconds >= 10) {
    achievementGlobalStats.totalReadingSeconds =
        achievementGlobalStats.totalReadingSeconds > UINT32_MAX - pendingReadingSeconds
            ? UINT32_MAX
            : achievementGlobalStats.totalReadingSeconds + pendingReadingSeconds;
  }

  auto achievement =
      makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, epub->getTitle(), epub->getAuthor(), achievementStats,
                                           "", achievementGlobalStats, BookStatsActivity::InitialPage::Achievement);
  if (!achievement) {
    LOG_ERR("ERS", "OOM: completion achievement screen");
    onGoHome();
    return;
  }
  stats.completionAchievementPending = false;
  stats.save(epub->getCachePath());
  activityManager.replaceActivity(std::move(achievement));
}

void EpubReaderActivity::onGoHomeRequested() {
  if (!epub || !SETTINGS.shouldTrackReadingStats()) {
    onGoHome();
    return;
  }
  recordCurrentPageReadingTime();
  goHomeOrShowCompletionAchievement();
}

void EpubReaderActivity::setBookCompleted(bool completed) {
  if (!epub || stats.isCompleted == completed) return;

  stats.isCompleted = completed;
  stats.completionAchievementPending = completed;
  stats.completionPromptDismissedAtHundred = false;
  if (completed && !stats.finishedDateManual && !stats.finishedDate.isValid()) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) stats.finishedDate = now.date;
  }

  if (completed) {
    if (SETTINGS.removeReadBooksFromRecents) RECENT_BOOKS.removeByPath(epub->getPath());
    if (SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath())) pendingReadFolderMove = true;
    if (globalStats.completedBooks < UINT32_MAX) ++globalStats.completedBooks;
  } else {
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
    }
    pendingReadFolderMove = false;
    if (globalStats.completedBooks > 0) --globalStats.completedBooks;
  }

  stats.save(epub->getCachePath());
  globalStats.save();
  syncFinishedBookIndex();
}
#endif

bool EpubReaderActivity::loadBook() {
  auto loadedEpub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!loadedEpub) {
    LOG_ERR("ERS", "Failed to allocate EPUB object");
    return false;
  }

  const bool uncached = !Storage.exists((loadedEpub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    disableFastInitialRefresh();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }

  bool loaded;
  {
#ifdef BOOK_PROFILE
    uint32_t load_start_ms = millis();
    uint8_t core = xPortGetCoreID();
    uint32_t psram_free_before = ESP.getFreePsram();
    uint32_t heap_free_before = ESP.getFreeHeap();
#endif
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = loadedEpub->load(true, SETTINGS.embeddedStyle == 0);
#ifdef BOOK_PROFILE
    uint32_t load_end_ms = millis();
    uint32_t psram_free_after = ESP.getFreePsram();
    uint32_t heap_free_after = ESP.getFreeHeap();
    LOG_INF("PROF", "phase=loadBook core=%d load_dur=%uus psram_free=%uB->%uB heap=%uB->%uB", core,
            load_end_ms - load_start_ms, psram_free_before, psram_free_after, heap_free_before, heap_free_after);
#endif
  }
  logMemAt("book_open");
  if (!loaded) {
    LOG_ERR("ERS", "Failed to load EPUB");
    return false;
  }
  epub = std::move(loadedEpub);

  ImageBlock::clearRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });
#ifdef BOARD_HAS_PSRAM
  ImageBlock::setPsramExtractor(epub.get(), [](void* ctx, const char* src, size_t& size) {
    return static_cast<Epub*>(ctx)->extractItemToPsram(src, &size);
  });
#endif

  epub->setupCacheDir();

#if defined(CROSSPOINT_TTF_READER)
  // Native-TTF page source (design §3.5). readerFontEngine is the documented
  // rollback switch (CrossPointSettings.h): BITMAP selects the legacy Section
  // path, so the runtime is only created when TTF is selected. A failed open
  // falls back to the legacy path too — second kill switch.
  ttf_ = SETTINGS.readerFontEngine == CrossPointSettings::READER_ENGINE_TTF
             ? makeUniqueNoThrow<freeink::book::TtfBookRuntime>()
             : nullptr;
  if (ttf_) {
    const std::string ttfCacheDir = epub->getCachePath() + "/ficache";
    if (!ttf_->open(bookPath.c_str(), ttfCacheDir.c_str())) {
      LOG_ERR("ERS", "TTF runtime open failed — using legacy reader path");
      ttf_.reset();
    } else {
      // Sync the FT faces' render mode before the first render/worker session
      // (onEnter ran before ttf_ existed on a fresh open).
      freeink::book::fontLoader.applyRenderMode(SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_CRISP);
    }
  }
#endif

  // ProgressManager is the single source of truth: openBook() loads the
  // on-disk record, seeds the manager's state, and hands the position back.
  uint16_t savedSpine = 0;
  uint16_t savedPage = 0;
  uint16_t savedPageCount = 0;
  uint32_t savedOffset = 0;
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    bool ttfHasGeneration = false;
    const bool progressLoaded =
        progressManager.openBookTtf(epub->getCachePath().c_str(), savedSpine, savedPage, savedPageCount,
                                    ttfSavedCharOffset, ttfSavedGeneration, ttfHasGeneration);
    if (progressLoaded) {
      const int spineCount = epub->getSpineItemsCount();
      if (spineCount <= 0 || savedSpine >= static_cast<uint16_t>(spineCount) ||
          (savedPageCount > 0 && savedPage >= savedPageCount && savedPage != UINT16_MAX)) {
        LOG_DBG("ERS", "Ignoring corrupt TTF progress: spine=%u page=%u/%u", savedSpine, savedPage, savedPageCount);
        savedSpine = 0;
        savedPage = 0;
        savedPageCount = 0;
        ttfHasGeneration = false;
      }
      currentSpineIndex = savedSpine;
      nextPageNumber = savedPage == UINT16_MAX ? 0 : savedPage;
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = savedPageCount;
      ttfSavedSpine = savedSpine;
      // Restore through pageForChar only when the generation still matches.
      // A legacy-shape record has no usable page mapping for TTF layout, so
      // it degrades to a chapter-start open (§7).
      ttfHasSavedPosition = ttfHasGeneration;
      if (!ttfHasSavedPosition) {
        nextPageNumber = 0;
        cachedChapterTotalPageCount = 0;
      }
      LOG_DBG("ERS", "Loaded TTF progress: spine %d, page %d, gen %u", currentSpineIndex, nextPageNumber,
              ttfSavedGeneration);
    }
  } else
#endif
  {
    const bool progressLoaded =
        progressManager.openBook(epub->getCachePath().c_str(), savedSpine, savedPage, savedPageCount, savedOffset);
    if (progressLoaded) {
      const int spineCount = epub->getSpineItemsCount();
      if (spineCount <= 0 || savedSpine >= static_cast<uint16_t>(spineCount) ||
          (savedPageCount > 0 && savedPage >= savedPageCount)) {
        LOG_DBG("ERS", "Ignoring corrupt progress: spine=%u page=%u/%u", savedSpine, savedPage, savedPageCount);
        savedSpine = 0;
        savedPage = 0;
        savedPageCount = 0;
      }
      currentSpineIndex = savedSpine;
      nextPageNumber = savedPage;
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = savedPageCount;
      if (savedPageCount > 0) {
        cachedVisibleTextOffset = savedOffset;
      }
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  loadCachedBookmarks();
  return true;
}

// Any reader chrome (toolbar sheet / panel / popup / open footnote) closes with
// the global back gesture instead of the gesture leaving the book; with no
// chrome the gesture falls through (caller pops / goes home as before).
bool EpubReaderActivity::handleHomeGesture() {
  if (!isChromeOpen()) return false;
  // Close only the topmost layer per gesture, matching the Back button's
  // stepped behavior: a sheet over an open footnote restores the sheet first
  // and leaves the footnote jump for the next gesture.
  if (overlay != Overlay::None) {
    closeOverlayToPage();
    return true;
  }
  if (footnoteDepth > 0) {
    restoreSavedPosition();
  }
  return true;
}

bool EpubReaderActivity::isChromeOpen() const {
  return overlay != Overlay::None || overlayPopup.isActive() || footnoteDepth > 0;
}

ChapterPosition EpubReaderActivity::chapterPosition() const {
  if (section) return {section->currentPage, section->estimatedTotalPages()};
  return {nextPageNumber, cachedChapterTotalPageCount};
}

int EpubReaderActivity::bookPercentFor(const ChapterPosition& position) const {
  if (!epub || epub->getBookSize() == 0 || !position.hasTotal()) return 0;
  // The page index can run past the chapter's estimated total while it is still
  // building, so the fraction is clamped before the cast.
  const float fraction = epub->calculateProgress(currentSpineIndex, position.chapterFraction());
  return static_cast<int>(std::clamp(fraction, 0.0f, 1.0f) * 100.0f + 0.5f);
}

void EpubReaderActivity::openReaderMenu() {
  pendingManualTurn = 0;
  if (usesToolbarMenu()) {
    // Reached from a child activity's result handler (footnotes, bookmarks,
    // go-to-percent... cancelled back to the menu), so the framebuffer holds
    // that screen, not the page: re-render the page and let renderBook() put
    // the toolbar on top. The in-reader fast path is openOverlay().
    overlay = Overlay::Toolbar;
    focusedTool = 0;
    panelHoldJumped = false;
    panelCursorShown = !mappedInput.hasTouch();
    if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
    toolbarUi->begin();
    discardOverlayPage();
    requestUpdate();
    return;
  }
#ifdef READING_STATS_ENABLED
  recordCurrentPageReadingTime();
  pageShownAtMs = 0;
#endif

  // Child screens (chapter list, text settings) release the section to free its
  // pagination buffers; chapterPosition() covers that with the cached position.
  const ChapterPosition position = chapterPosition();
  const int bookProgressPercent = bookPercentFor(position);

  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, epub->getTitle(), position.displayPage(),
                                               position.totalPages, bookProgressPercent, SETTINGS.orientation,
                                               !currentPageFootnotes.empty(), !cachedBookmarks.empty()),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        if (SETTINGS.orientation != menu.orientation) {
          applyOrientation(menu.orientation);
        }
        toggleAutoPageTurn(menu.pageTurnOption);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      });
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::prefetchNextChapterDuringDisplay() {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    ttfPrefetchTick();
    return;
  }
#endif
#if defined(BOARD_HAS_PSRAM) && defined(ESP_PLATFORM)
#ifdef BOOK_PROFILE
  const auto prefetchStart = millis();
#endif
  if (!epub) return;
  if (buildHeapPaused) return;
  const int nextSpine = currentSpineIndex + 1;
  if (nextSpine >= epub->getSpineItemsCount()) return;

  // Only prefetch if the next chapter hasn't been cached yet
  if (nextSectionPrefetch && nextSectionSpineIndex == nextSpine) {
    // Continue the existing prefetch build
    if (nextSectionPrefetch->isBuilding() && !nextSectionPrefetch->isBuildComplete()) {
      nextSectionPrefetch->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK);
#ifdef BOOK_PROFILE
      LOG_DBG("PROF", "phase=prefetch_continue spine=%d pages_built=%d dur=%lums core=%d", nextSpine,
              nextSectionPrefetch->pageCount, millis() - prefetchStart, xPortGetCoreID());
#endif
    }
    return;
  }

  // Start a new prefetch for the next chapter
  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
  nextSectionPrefetch = std::make_unique<Section>(epub, nextSpine, renderer);
  nextSectionSpineIndex = nextSpine;

  // Try loading existing cache first; if missing, start a build
  if (nextSectionPrefetch->loadSectionFile(renderSpec)) {
#ifdef BOOK_PROFILE
    LOG_DBG("PROF", "phase=prefetch_cache_hit spine=%d dur=%lums core=%d", nextSpine, millis() - prefetchStart,
            xPortGetCoreID());
#endif
  } else {
    nextSectionPrefetch->startBuild(renderSpec);
    nextSectionPrefetch->buildSomeMore(BUILD_PAGES_PER_CHUNK);
#ifdef BOOK_PROFILE
    LOG_DBG("PROF", "phase=prefetch_cold_build spine=%d pages_built=%d dur=%lums core=%d", nextSpine,
            nextSectionPrefetch->pageCount, millis() - prefetchStart, xPortGetCoreID());
#endif
  }
#endif
}

void EpubReaderActivity::showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

void EpubReaderActivity::openDictionaryWordSelect(int touchX, int touchY, TouchLongPressMode mode) {
  if (mode == TouchLongPressMode::Dictionary && SETTINGS.dictionaryName[0] == '\0') {
    showDictionaryMessage = true;
    dictionaryMessageTtf = false;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    // TTF word selection: prebuilt boxes from engine run geometry
    // (TtfWordSelect). Run text lives in the runtime's scratch arena only
    // for this block — buildTtfWordSelectData copies everything out, so the
    // mark is released before the child activity runs.
    RenderLock lock;  // the render task owns/uses the scratch arena
    const size_t scratchMark = ttf_->scratch().mark();
    const auto showTtfDictionaryError = [this] {
      showDictionaryMessage = true;
      dictionaryMessageTtf = true;
      dictionaryMessageTime = millis();
      requestUpdate();
    };
    freeink::book::Page page{};
    if (!ttf_->readPage(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(ttfPage), &page)) {
      ttf_->scratch().release(scratchMark);
      LOG_ERR("ERS", "TTF dictionary: page read failed (spine %d page %d)", currentSpineIndex, ttfPage);
      showTtfDictionaryError();
      return;
    }
    freeink::book::LayoutParams params;
    ttf_->makeLayoutParams(renderer, params, automaticPageTurnActive);
    freeink::book::TtfWordSelectData data;
    if (params.font == nullptr ||
        !freeink::book::buildTtfWordSelectData(page, *static_cast<freeink::book::FontChain*>(params.font), data)) {
      ttf_->scratch().release(scratchMark);
      LOG_ERR("ERS", "TTF dictionary: word extraction failed");
      showTtfDictionaryError();
      return;
    }
    // The page's footnote list was captured during the last render (§3.5
    // item 8); the selector resolves bare numeric markers against it.
    data.footnotes = currentPageFootnotes;
    ttf_->scratch().release(scratchMark);

    const DictionaryWordSelectActivity::PageRenderFn renderFn{this, &EpubReaderActivity::renderTtfSelectorPage};
    auto selector = makeUniqueNoThrow<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(data), renderFn,
                                                                    touchX, touchY, mode);
    if (!selector) {
      LOG_ERR("ERS", "OOM: dictionary word selector");
      showTtfDictionaryError();
      return;
    }
    startActivityForResult(std::move(selector), [this](const ActivityResult& result) {
      if (!result.isCancelled && std::holds_alternative<FootnoteResult>(result.data)) {
        navigateToHref(std::get<FootnoteResult>(result.data).href, /*savePosition=*/true);
      }
      requestUpdate();
    });
    return;
  }
#endif
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  auto selector = makeUniqueNoThrow<DictionaryWordSelectActivity>(
      renderer, mappedInput, std::move(page), orientedMarginLeft, orientedMarginTop, touchX, touchY, mode);
  if (!selector) {
    LOG_ERR("ERS", "OOM: dictionary word selector");
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(selector), [this](const ActivityResult& result) {
    if (!result.isCancelled && std::holds_alternative<FootnoteResult>(result.data)) {
      navigateToHref(std::get<FootnoteResult>(result.data).href, /*savePosition=*/true);
    }
    requestUpdate();
  });
}

void EpubReaderActivity::loop() {
  if (!epub) {
    finish();
    return;
  }

  retryDeferredOverlayPush();

  // Someone else turned the screen while this reader was stacked (the control
  // center's orientation tile). Reflow before the next render, or the page
  // would be drawn with a layout built for the previous frame size.
  if (appliedOrientation != SETTINGS.orientation) {
    applyOrientation(SETTINGS.orientation);
    requestUpdate();
    return;
  }

  // A loop-task build tick can occupy the RenderLock for hundreds of ms. Input
  // is only polled between loop passes, and the touch controller reports just
  // the current frame, so a gesture completed inside a tick stall never
  // reaches the reader. Defer background layout while input is active or an
  // edge is still pending.
  const bool inputPriority = mappedInput.rawInputPriority();

  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  if (!inputPriority && section && !section->isBuilding() && !RenderLock::peek() && renderer.hasFrameBuffer() &&
      lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS &&
      ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC &&
      (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
    RenderLock lock;
    if (section && !section->isBuilding() &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      idlePrewarmSpine = currentSpineIndex;
      idlePrewarmPage = section->currentPage;
      const int nextPage = section->currentPage + 1;
      if (nextPage < static_cast<int>(section->pageCount)) {
        if (const auto p = section->loadPage(nextPage)) {
          if (auto* fcm = renderer.getFontCacheManager()) {
            const auto t0 = millis();
            auto scope = fcm->createPrewarmScope();
            p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);
            scope.endScanAndPrewarm();
            LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
          }
        }
      }
    }
  }

  if (!inputPriority && section && !section->isBuilding() && section->isPartial() && !RenderLock::peek() &&
      buildViewportWidth > 0 && !partialRebuildStartFailed &&
      section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
    RenderLock lock;
    const ReaderRenderSpec buildSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
    if (!section->startBuild(buildSpec)) {
      partialRebuildStartFailed = true;
      LOG_ERR("ERS", "Failed to start deferred partial extension build");
    } else {
      LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
              section->pageCount);
    }
  }

  if (!inputPriority && section && section->isBuilding() && !RenderLock::peek() &&
      (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD) &&
      buildTickHeapGate()) {
    RenderLock lock;
    if (section->isBuilding() && buildTickHeapGate()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        section.reset();
        requestUpdate();
      } else if (section->isBuildComplete() && applyDeferredReposition()) {
        requestUpdate();
      }
    }
  }

#if defined(CROSSPOINT_TTF_READER)
  if (!inputPriority && ttf_ && !RenderLock::peek() && buildTickHeapGate()) {
    RenderLock lock;
    if (ttf_ && buildTickHeapGate()) {
      ttfBackgroundBuildTick();
    }
  }
#endif

  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  clearEndOfBookOptionsIfNeeded();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

#ifdef READING_STATS_ENABLED
  if (atEndOfBook || stats.isCompleted) {
#else
  if (atEndOfBook) {
#endif
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  // The open overlay owns the touch latch; page-turn tap detection must not
  // consume a tap that FUI will route to the sheet.
  const auto touch = overlay == Overlay::None ? ReaderUtils::detectTouchPageTurn(renderer, mappedInput)
                                              : ReaderUtils::TouchPageTurn{false, false, 0};

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

#if FREEINK_CAP_FRONTLIGHT
  // Frontlight side-swipe gestures (left edge: warmth, right edge: brightness).
  // Runs after detectTouchPageTurn() so the swipe state is available, and before
  // the overlay + page-turn path so a handled side-swipe consumes the touch
  // without also turning a page. Independent of SETTINGS.touchReaderControls.
  // Gated on no overlay/end-of-book menu so the gesture can't fire while those
  // surfaces own input.
  if (SETTINGS.frontlightSideGestures && Frontlight.present() && overlay == Overlay::None && !endOfBookMenuActive()) {
    if (handleSideSwipeFrontlight()) {
      return;
    }
  }
#endif

  // The toolbar reader menu owns all input while shown, ahead of the automatic page turn
  // below: the More panel's rate popup switches automatic turning on and leaves the panel
  // open, so the timer must neither flip the page under it nor eat the panel's next
  // Confirm/Back release.
  if (overlay != Overlay::None) {
    if (usesToolbarMenu()) {
      // Hold the interval at zero elapsed so closing the panel starts a fresh one.
      lastPageTurnTime = millis();
      handleOverlayInput();
      return;
    }
    // The style was switched off while an overlay was up (Settings reached via
    // the More panel); fall back to the clean page.
    overlay = Overlay::None;
    discardOverlayPage();
    requestUpdate();
    return;
  }

  switch (mappedInput.homeButtonAction()) {
    case HomeButtonAction::ReaderMenu:
    case HomeButtonAction::Bookmark:
    case HomeButtonAction::Sync:
    case HomeButtonAction::Dictionary:
    case HomeButtonAction::Footnotes:
      automaticPageTurnActive = false;
      break;
    default:
      break;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }

    if (!section
#if defined(CROSSPOINT_TTF_READER)
        && !ttf_
#endif
    ) {
      requestUpdate();
      return;
    }

    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      requestUpdate();
      return;
    }
  }

  // While the end-of-book suggestion menu is up it owns Confirm/Back/navigation, so it
  // gets this tick's input first and the long-press shortcuts below stay inert behind it
  // -- a hold there must not drop a bookmark onto the suggestion screen or paint the
  // dictionary word picker over it. Anything the menu does not handle (long-press Back to
  // the file browser, say) still falls through to the regular handlers.
  if (handleEndOfBookMenu()) {
    return;
  }

  if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF && mappedInput.hasTouch() &&
      SETTINGS.touchLongPressAction != CrossPointSettings::TOUCH_LP_IGNORE && !showDictionaryMessage &&
      !automaticPageTurnActive) {
    int lx = 0, ly = 0;
    if (mappedInput.wasScreenLongPress(lx, ly)) {
      const auto mode = (SETTINGS.touchLongPressAction == CrossPointSettings::TOUCH_LP_FOOTNOTE)
                            ? TouchLongPressMode::Footnote
                            : TouchLongPressMode::Dictionary;
      openDictionaryWordSelect(lx, ly, mode);
      return;
    }
  }

  const bool endOfBookMenuOpen = endOfBookMenuActive();
#if FREEINK_CAP_MENU_BUTTON
  // Long-press Confirm runs the user-selected long-press function. Boards
  // without any Confirm button (physical or synthesized) drop this entirely.
  const unsigned long confirmHoldMs = confirmLongPressThreshold();
  // wasLongPressed() suppresses the release that follows it, so leave it unpolled while
  // the end-of-book menu owns Confirm -- otherwise the menu never sees that release.
  const bool confirmLongPressed = !endOfBookMenuOpen && confirmHoldMs != 0 &&
                                  mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, confirmHoldMs);
  if (confirmLongPressed) {
    switch (static_cast<HomeButtonAction>(SETTINGS.homeButtonLongPressAction)) {
      case HomeButtonAction::Bookmark:
        addBookmark();
        showBookmarkMessage = true;
        bookmarkMessageTime = millis();
        requestUpdate();
        break;
      case HomeButtonAction::Sync:
        if (launchKOReaderSync()) {
          return;
        }
        break;
      case HomeButtonAction::Dictionary:
        openDictionaryWordSelect();
        return;
      case HomeButtonAction::ReaderMenu:
      case HomeButtonAction::Ignore:
      default:
        break;
    }
  }
#endif

  if (!endOfBookMenuOpen) {
    switch (mappedInput.homeButtonAction()) {
      case HomeButtonAction::Bookmark:
        if (!showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        return;
      case HomeButtonAction::Sync:
        if (launchKOReaderSync()) return;
        break;
      case HomeButtonAction::Dictionary:
        if (!showDictionaryMessage) openDictionaryWordSelect();
        return;
      case HomeButtonAction::ReaderMenu:
        if (usesToolbarMenu() && (section
#if defined(CROSSPOINT_TTF_READER)
                                  || ttf_
#endif
                                  ))
          openOverlay(Overlay::Toolbar);
        else
          openReaderMenu();
        return;
      default:
        break;
    }
  }

  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);

  if (confirmReleased || ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    // Toolbar style: the page is on screen and in the framebuffer, so paint the
    // toolbar over it (one refresh) instead of pushing a full-screen menu.
    if (usesToolbarMenu() && section) {
      pendingManualTurn = 0;
      openOverlay(Overlay::Toolbar);
    } else {
      openReaderMenu();
    }
  }

  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (handleBackNavigation()) {
    return;
  }

  if ((!endOfBookMenuOpen && mappedInput.homeButtonAction() == HomeButtonAction::Footnotes) ||
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
       mappedInput.wasReleased(MappedInputManager::Button::Power) &&
       !mappedInput.wasReleased(MappedInputManager::Button::Down))) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  constexpr unsigned long kMinManualTurnGapMs = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs;
  if (pendingManualTurn != 0 && !turnGuardActive) {
    if (!section
#if defined(CROSSPOINT_TTF_READER)
        && !ttf_
#endif
    ) {
      pendingManualTurn = 0;
      return;
    }
    const bool forward = pendingManualTurn > 0;
    pendingManualTurn = 0;
    pageTurn(forward);
    requestUpdate();
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs >= ReaderUtils::SKIP_HOLD_MS;
  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    skipPages(nextTriggered ? 1 : -1);
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  if (!section
#if defined(CROSSPOINT_TTF_READER)
      && !ttf_
#endif
  ) {
    requestUpdate();
    return;
  }

  if (turnGuardActive) {
    pendingManualTurn = prevTriggered ? -1 : 1;
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
  requestUpdate();
}

void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) return;
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return;

  percent = clampPercent(percent);

  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) targetSize = bookSize - 1;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) return;

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
#ifdef READING_STATS_ENABLED
  recordCurrentPageReadingTime();
  pageShownAtMs = 0;
#endif
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (result.isCancelled) {
      openReaderMenu();
    } else {
      const auto& sync = std::get<ProgressChangeResult>(result.data);

      if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
        RenderLock lock;
        clearDeferredReposition();
        if (section && currentSpineIndex == sync.spineIndex) {
          const auto page = section->getPageForVisibleTextOffset(sync.visibleTextOffset);
          section->currentPage = page.value_or(std::max(0, sync.page));
        } else {
          currentSpineIndex = sync.spineIndex;
          pendingOffsetJump = sync.visibleTextOffset;
          nextPageNumber = std::max(0, sync.page);
          section.reset();
        }
        requestUpdate();
        return;
      }

      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      RenderLock lock;
      clearDeferredReposition();

      if (currentSpineIndex != targetSpineIndex) {
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
      requestUpdate();
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      // Release the section while the chapter list is up (mirrors the
      // TEXT_SETTINGS path): picking a chapter resets it anyway, and its
      // tens-of-KB footprint is the difference between the chapter list
      // holding its CJK glyph arena (RAM-only repaints) and re-reading
      // glyphs from SD on every row step. Cancel restores via the same
      // cached-position rebuild TEXT_SETTINGS uses.
      {
        RenderLock lock;
        if (section) {
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        section.reset();
      }
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, spineIdx),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const auto& chapterResult = std::get<ChapterResult>(result.data);
            RenderLock lock;
            clearDeferredReposition();
            currentSpineIndex = chapterResult.spineIndex;
            pendingAnchor = chapterResult.anchor;
            nextPageNumber = 0;
            section.reset();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 openReaderMenu();
                                 return;
                               }
                               const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                               navigateToHref(footnoteResult.href, true);
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
      startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                    TextSettingsActivity::Tab::Family),
                             [this](const ActivityResult&) {
                               {
                                 RenderLock lock;
                                 if (section) {
                                   rememberCurrentContentOffset();
                                   cachedSpineIndex = currentSpineIndex;
                                   cachedChapterTotalPageCount = section->pageCount;
                                   nextPageNumber = section->currentPage;
                                 }
                                 section.reset();
                               }
                               openReaderMenu();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NIGHT_MODE:
      // Handled in-place by EpubReaderMenuActivity so its On/Off value updates
      // without closing the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::FRONTLIGHT:
      // Handled in-place by EpubReaderMenuActivity using the live frontlight HAL.
      break;
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const int initialPercent = bookPercentFor(chapterPosition());
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
            } else {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult&) { openReaderMenu(); });
          break;
        }
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHomeRequested();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock;
        if (epub) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = 0;
          uint16_t backupPageCount = 0;
#if defined(CROSSPOINT_TTF_READER)
          // TTF books hold no Section mirror: capture the generation-tagged
          // position BEFORE the runtime closes, so the clear saves a TTF
          // record instead of a legacy page=0 one (which would drop the
          // reader back to chapter start on reopen).
          bool ttfPosition = false;
          uint32_t ttfCharOffset = 0;
          uint32_t ttfGen = 0;
          if (ttf_ && ttfGenerationValid) {
            ttfPosition = true;
            backupPage = static_cast<uint16_t>(ttfPage);
            backupPageCount = static_cast<uint16_t>(ttfPageCount);
            ttfCharOffset = ttfCurrentCharStart;
            ttfGen = ttfGeneration;
          }
#endif
          if (section) {
            backupPage = section->currentPage;
            backupPageCount = section->pageCount;
            section.reset();
          }
          // Close every handle into the cache dir BEFORE removeDir: the
          // prefetch worker's own FIBP writer/reader and the TTF runtime keep
          // ficache files open, and a single open file fails the recursive
          // removal — leaving the whole epub_<hash> folder (ficache included)
          // behind on SD.
#if defined(CROSSPOINT_TTF_READER)
          stopFibpWorker();
          if (ttf_) {
            ttf_->close();
          }
#endif
          epub->clearCache();
          epub->setupCacheDir();
          // Single-writer rule (design §4.7): the cache-clear save also goes
          // through the saver so its state matches what is on disk.
          bool saved = false;
#if defined(CROSSPOINT_TTF_READER)
          if (ttfPosition) {
            saved = progressManager.saveNowTtf(epub->getCachePath().c_str(), backupSpine, backupPage, backupPageCount,
                                               ttfCharOffset, ttfGen);
          }
#endif
          if (!saved && !progressManager.saveNow(epub->getCachePath().c_str(), backupSpine, backupPage, backupPageCount,
                                                 false, 0)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHomeRequested();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock;
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
#ifdef READING_STATS_ENABLED
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      recordCurrentPageReadingTime();
      BookReadingStats displayStats = stats;
      if (SETTINGS.shouldTrackReadingStats()) {
        displayStats.totalReadingSeconds += sessionReadingSeconds;
      }
      auto statsMenu = makeUniqueNoThrow<ReadingStatsMenuActivity>(
          renderer, mappedInput, displayStats, epub->getTitle(), epub->getCachePath(), epub->getAuthor());
      if (!statsMenu) {
        LOG_ERR("ERS", "OOM: reading stats menu");
        break;
      }
      startActivityForResult(std::move(statsMenu), [this](const ActivityResult& result) {
        if (epub && SETTINGS.shouldTrackReadingStats()) handleBookStatsReturn();
        if (std::holds_alternative<ClearPaceResult>(result.data) && epub) {
          stats.clearWpmStats();
          stats.save(epub->getCachePath());
        }
        openReaderMenu();
      });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_STATS: {
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_BOOK_STATS), epub->getTitle()),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            if (SETTINGS.shouldTrackReadingStats() && epub && BookReadingStats::remove(epub->getCachePath())) {
              stats = BookReadingStats{};
              sessionReadingSeconds = 0;
              hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);
            }
            openReaderMenu();
          });
      break;
    }
#endif
  }
}

unsigned long EpubReaderActivity::confirmLongPressThreshold() const {
  switch (static_cast<HomeButtonAction>(SETTINGS.homeButtonLongPressAction)) {
    case HomeButtonAction::Bookmark:
    case HomeButtonAction::Dictionary:
      return ReaderUtils::BOOKMARK_HOLD_MS;
    case HomeButtonAction::Sync:
      return KOREADER_STORE.hasCredentials() ? ReaderUtils::GO_HOME_MS : 0;
    case HomeButtonAction::ReaderMenu:
    case HomeButtonAction::Ignore:
    default:
      return 0;
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;

  RenderLock renderLock;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;

  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos;
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // The synchronous save below routes through the saver (single writer,
  // design §4.7); saveNow also records the position so the background tick
  // never rewrites it after the epub has been released (design §4.4 KOReader
  // exception).
  {
    std::optional<uint32_t> savedOffset;
    if (section && currentPage >= 0 && currentPage < section->pageCount) {
      savedOffset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                        ? currentPageVisibleOffset
                        : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
    }
#if defined(CROSSPOINT_TTF_READER)
    // The TTF path records the generation-tagged char offset so the record
    // keeps its restore anchor; the legacy path keeps the visible-text offset.
    const bool saved = ttf_ ? progressManager.saveNowTtf(epub->getCachePath().c_str(), currentSpineIndex, currentPage,
                                                         totalPages, ttfCurrentCharStart, ttfGeneration)
                            : progressManager.saveNow(epub->getCachePath().c_str(), currentSpineIndex, currentPage,
                                                      totalPages, savedOffset.has_value(), savedOffset.value_or(0));
#else
    const bool saved = progressManager.saveNow(epub->getCachePath().c_str(), currentSpineIndex, currentPage, totalPages,
                                               savedOffset.has_value(), savedOffset.value_or(0));
#endif
    if (!saved) {
      LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
      pendingSyncSaveError = true;
      requestUpdate();
      return true;
    }
  }

  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    if (section) {
      nextPageNumber = section->currentPage;
    }
    discardOverlayPage();
    ImageBlock::releaseRenderCache();
    ImageBlock::setExtractor(nullptr, nullptr);
#ifdef BOARD_HAS_PSRAM
    ImageBlock::setPsramExtractor(nullptr, nullptr);
#endif
    section.reset();
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseSdFontCaches();
    }
    // No rendering may run while the chapter mapper borrows the framebuffer.
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
    }
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, localPos, std::move(localKoPos), std::move(localChapterName)
#if defined(CROSSPOINT_TTF_READER)
                                                                                 ,
      ttfGeneration, ttf_ != nullptr
#endif
      ));
  return true;
}

void EpubReaderActivity::applyInitialOrientation() {
  ReaderActivity::applyInitialOrientation();
  appliedOrientation = SETTINGS.orientation;
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // Also runs when SETTINGS already holds the new value but this layout was
  // built for the old one — that is what an external change looks like here.
  if (SETTINGS.orientation == orientation && appliedOrientation == orientation) {
    return;
  }

  RenderLock lock(*this);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    // Geometry change -> new generation -> new caches; the position rides the
    // chapter char offset (design section 3.5).
    ttfInvalidateCaches();
  }
#endif

  if (SETTINGS.orientation != orientation) {
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
  }
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  appliedOrientation = orientation;
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  const bool wasActive = automaticPageTurnActive;
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
  } else {
    lastPageTurnTime = millis();
    pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
    automaticPageTurnActive = true;
  }

#if defined(CROSSPOINT_TTF_READER)
  if (ttf_ && automaticPageTurnActive != wasActive) {
    RenderLock lock;
    ttfInvalidateCaches();
  }
#endif

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    RenderLock lock;
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

bool EpubReaderActivity::pageTurn(bool isForwardTurn) {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) return ttfPageTurn(isForwardTurn);
#endif
  if (!section) return false;
  {
    RenderLock lock;
    clearDeferredReposition();
  }

#ifdef READING_STATS_ENABLED
  uint32_t dwellSeconds = 0;
  const bool haveDwell = currentPageReadingSecondsForStats(dwellSeconds);
  if (SETTINGS.shouldTrackReadingStats()) {
    recordCurrentPageReadingTime();
    if (isForwardTurn && haveDwell) {
      recordForwardPagePaceSample(dwellSeconds, currentPageWordsOnPage);
      if (stats.totalPagesTurned < UINT32_MAX) stats.totalPagesTurned++;
      if (globalStats.totalPagesTurned < UINT32_MAX) globalStats.totalPagesTurned++;
      if (sessionPageTurns < UINT16_MAX) sessionPageTurns++;
    }
  }
#endif

  if (isForwardTurn) {
#ifdef READING_STATS_ENABLED
    // A declined 100%-completion prompt applies only until the reader moves
    // forward again; otherwise it would suppress the prompt on a later exit.
    stats.completionPromptDismissedAtHundred = false;
#endif
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
      lastPageTurnTime = millis();
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
      // Promote a prefetched next-section if it matches the new chapter
      if (nextSectionPrefetch && nextSectionSpineIndex == currentSpineIndex) {
        section = std::move(nextSectionPrefetch);
        nextSectionSpineIndex = -1;
      } else {
        section.reset();
      }
      lastPageTurnTime = millis();
    } else {
      currentSpineIndex = epub->getSpineItemsCount();
#ifdef READING_STATS_ENABLED
      // Crossing past the last readable page is definitive completion.
      if (SETTINGS.shouldTrackReadingStats() && !stats.isCompleted) setBookCompleted(true);
#endif
      // The EOB screen bypasses the section render path, so persist the
      // terminal position synchronously instead of waiting for the gate.
      if (!progressManager.saveNow(epub->getCachePath().c_str(), /*spineIndex=*/epub->getSpineItemsCount(), 0, 0, false,
                                   0)) {
        LOG_ERR("ERS", "Failed to save end-of-book progress");
      }
      lastPageTurnTime = millis();
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
      lastPageTurnTime = millis();
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      currentSpineIndex--;
      section.reset();
      // Clear prefetch if it doesn't match the new current chapter
      if (nextSectionSpineIndex != currentSpineIndex) {
        nextSectionPrefetch.reset();
        nextSectionSpineIndex = -1;
      }
      lastPageTurnTime = millis();
    } else {
      return false;
    }
  }

#ifdef READING_STATS_ENABLED
  pageShownAtMs = millis();
#endif
  logMemAt("page_turn");
  return true;
}

bool EpubReaderActivity::skipPages(int amount) {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    ttfFrameRenderComplete.store(false, std::memory_order_release);
    if (amount > 0) {
      if (currentSpineIndex + 1 >= epub->getSpineItemsCount()) return false;
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
      ttfRestoreLastPage = false;
      return true;
    }
    if (ttfPage > 0) {
      ttfPage = 0;
      return true;
    }
    if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      ttfRestoreLastPage = false;
      return true;
    }
    return false;
  }
#endif
  if (!section) return false;
  if (amount > 0) {
#ifdef READING_STATS_ENABLED
    // A forward skip is still a forward turn: clear a declined 100% prompt.
    stats.completionPromptDismissedAtHundred = false;
#endif
    RenderLock lock;
    nextPageNumber = 0;
    currentSpineIndex++;
    // Promote prefetched section if it matches, otherwise discard stale prefetch
    if (nextSectionPrefetch && nextSectionSpineIndex == currentSpineIndex) {
      section = std::move(nextSectionPrefetch);
      nextSectionSpineIndex = -1;
    } else {
      nextSectionPrefetch.reset();
      nextSectionSpineIndex = -1;
    }
    section.reset();
#ifdef READING_STATS_ENABLED
    if (SETTINGS.shouldTrackReadingStats() && !stats.isCompleted && currentSpineIndex == epub->getSpineItemsCount())
      setBookCompleted(true);
#endif
    if (currentSpineIndex == epub->getSpineItemsCount()) {
      // The EOB screen bypasses the section render path, so persist the
      // terminal position synchronously when a skip crosses it.
      if (!progressManager.saveNow(epub->getCachePath().c_str(), /*spineIndex=*/epub->getSpineItemsCount(), 0, 0, false,
                                   0)) {
        LOG_ERR("ERS", "Failed to save end-of-book progress");
      }
    }
    return true;
  } else {
    if (section->currentPage > 0) {
      section->currentPage = 0;
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      section.reset();
      // Clear prefetch if it doesn't match the new current chapter
      if (nextSectionSpineIndex != currentSpineIndex) {
        nextSectionPrefetch.reset();
        nextSectionSpineIndex = -1;
      }
      return true;
    }
  }
  return false;
}

bool EpubReaderActivity::isAtEndOfBook() const { return epub && currentSpineIndex >= epub->getSpineItemsCount(); }

void EpubReaderActivity::onReturnFromEndOfBook() {
  if (epub && epub->getSpineItemsCount() > 0) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = 0;
    pendingPageJump = std::numeric_limits<uint16_t>::max();
  }
}

bool EpubReaderActivity::preventAutoSleep() {
#if defined(CROSSPOINT_TTF_READER)
  // Live background builds are real work: the input-idle governor would
  // otherwise drop to LOW_POWER_FREQ mid-index (measured: 2.6 s/page spine
  // builds, and a task-WDT IDLE0 starvation reboot when one computation
  // stretch at low frequency exceeded the watchdog window). The worker
  // self-exits once every spine has been attempted, so this is bounded.
  if (fibpWorker_ != nullptr && fibpBegun_ && fibpWorker_->active()) return true;
  // A heap-paused session is stalled, not working: no pump runs below the
  // free-heap floors, so it must not hold low-power/auto-sleep hostage
  // (mirrors skipLoopDelay()).
  if (ttf_ != nullptr && ttf_->sessionActive() && !buildHeapPaused) return true;
#endif
  return section != nullptr && section->isBuilding();
}

bool EpubReaderActivity::skipLoopDelay() {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    return ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex)) && ttf_->sessionActive() && !buildHeapPaused;
  }
#endif
  return section && section->isBuilding() && !buildHeapPaused &&
         (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
}

void EpubReaderActivity::renderBook() {
#if defined(CROSSPOINT_TTF_READER)
  // Native-TTF page source (design §3.5): a fully separate render path so the
  // legacy Section pipeline below stays untouched.
  if (ttf_) {
    renderBookTtf();
    return;
  }
#endif
#ifdef BOOK_PROFILE
  uint32_t render_book_start_ms = millis();
  uint8_t core = xPortGetCoreID();
  uint32_t psram_free_before = ESP.getFreePsram();
  uint32_t heap_free_before = ESP.getFreeHeap();
#endif
  if (!epub) return;
  // Runs under the render task's RenderLock; catches every requestUpdate()
  // exit from the overlay while its deferred chrome refresh is still pending.
  settleOverlayRefresh();

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  if (currentSpineIndex == epub->getSpineItemsCount()) {
#ifdef BOOK_PROFILE
    LOG_INF("PROF", "phase=renderBook exit: no more spines core=%d", core);
#endif
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);

  if (!section) {
    // Chapter changed or first load — clear stale prefetch
    nextSectionPrefetch.reset();
    nextSectionSpineIndex = -1;
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    partialRebuildStartFailed = false;

    const bool cacheLoaded = section->loadSectionFile(renderSpec);
#ifdef BOOK_PROFILE
    uint32_t psram_free_after = ESP.getFreePsram();
    uint32_t heap_free_after = ESP.getFreeHeap();
    LOG_INF("PROF", "phase=renderBook_cacheLoad core=%d cache_loaded=%s psram_free=%uB->%uB heap=%uB->%uB", core,
            cacheLoaded ? "yes" : "no", psram_free_before, psram_free_after, heap_free_before, heap_free_after);
#endif
    if (cacheLoaded) {
      cachedChapterTotalPageCount = 0;
      cachedVisibleTextOffset.reset();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    const bool explicitOffsetJump = pendingOffsetJump.has_value();
    const std::optional<uint32_t> offsetJump =
        explicitOffsetJump ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        pagesUntilFullRefresh = 1;
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();
          showBuildError();
          return;
        }
#ifdef BOOK_PROFILE
        uint32_t psram_free_after = ESP.getFreePsram();
        uint32_t heap_free_after = ESP.getFreeHeap();
        LOG_INF("PROF", "phase=renderBook_createSection core=%d create_psram_free=%uB->%uB heap_free=%uB->%uB", core,
                psram_free_before, psram_free_after, heap_free_before, heap_free_after);
#endif
        loan.end();
      } else {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            pagesUntilFullRefresh = 1;
          }
          buildPopupPending = !showPopup;
          const unsigned long buildStartMs = millis();
          bool started;
          {
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(renderer, pagesUntilFullRefresh); });
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            buildPopupPending = false;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              showBuildPopup(renderer, pagesUntilFullRefresh);
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              buildPopupPending = false;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) section->currentPage = 0;
    }

    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        clearDeferredReposition();
      }
    }
    if (explicitOffsetJump) {
      clearDeferredReposition();
    }
    pendingOffsetJump.reset();

    if (!pendingAnchor.empty()) {
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;

    currentPageVisibleOffset = p->visibleTextOffset;
    currentPageFootnotes = std::move(p->footnotes);
#ifdef READING_STATS_ENABLED
    currentPageWordsOnPage = p->wordCount();
#endif

    // The overlay and non-tiled grayscale renderer share the renderer's single
    // stored-BW slot. Release the old page snapshot before renderContents()
    // needs that slot, then snapshot the newly rendered page below.
    discardOverlayPage();

#ifdef BOOK_PROFILE
    uint8_t render_core = xPortGetCoreID();
    uint32_t render_start_ms = millis();
    uint32_t psram_free_before_r = ESP.getFreePsram();
    uint32_t heap_free_before_r = ESP.getFreeHeap();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    uint32_t psram_free_after_r = ESP.getFreePsram();
    uint32_t heap_free_after_r = ESP.getFreeHeap();
    uint32_t render_end_ms = millis();
    LOG_INF("PROF", "phase=renderContents core=%d render_dur=%uus psram_free=%uB->%uB heap=%uB->%uB", render_core,
            render_end_ms - render_start_ms, psram_free_before_r, psram_free_after_r, heap_free_before_r,
            heap_free_after_r);
    LOG_DBG("ERS", "Rendered page in %dms", render_end_ms - render_start_ms);
#else
    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
#endif
    lastRenderCompleteMs = millis();
#ifdef READING_STATS_ENABLED
    pageShownAtMs = millis();
#endif
  }

  {
    // One call per render: the manager keeps the in-memory position current
    // and gates the disk write itself (changed + interval, or low battery —
    // §4.2/§4.5). Unchanged renders are no-ops inside the manager.
    LOG_DBG("PRG", "caller: save spine=%u page=%u/%u offset=%d", currentSpineIndex, section->currentPage,
            section->estimatedTotalPages(), currentPageVisibleOffset.has_value() ? 1 : 0);
    progressManager.save(currentSpineIndex, section->currentPage, section->estimatedTotalPages(),
                         currentPageVisibleOffset.has_value(), currentPageVisibleOffset.value_or(0));
  }
  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, dictionaryMessageTtf ? tr(STR_DICT_TTF_UNSUPPORTED) : tr(STR_DICT_NO_DICT_SET));
  }

  // Toolbar menu: overlay the toolbar / panel on top of the freshly rendered page.
  if (overlay != Overlay::None && usesToolbarMenu()) {
    // The page just re-rendered under the overlay: refresh the snapshot that
    // backs panel->toolbar restores (any previous copy is stale).
    discardOverlayPage();
    overlayPageStored = renderer.storeBwBuffer();
    renderOverlay();
    // An open option picker rides on top of the freshly drawn panel.
    if (overlayPopup.isActive()) overlayPopup.render(renderer);
    // FAST, same as openOverlay: HALF's inverting pass flashes the sheet
    // (white, in night mode) on every repaint under an open panel. Any AA
    // residue a FAST differential leaves under the chrome has not shown in
    // practice; restore a HALF cleanup here if text ever visibly ghosts
    // through the sheet (see #2190 for the mechanism).
    pushOverlayRefresh();
  }
}

// ── Native-TTF render path (design §3.5, CROSSPOINT_TTF_READER builds) ───────

#if defined(CROSSPOINT_TTF_READER)

void EpubReaderActivity::ttfSaveProgress() {
  // progressManager stays the single writer; this reports the
  // generation-tagged record shape (charOffset + generation) instead of a
  // visible-text offset.
  progressManager.saveTtf(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(ttfPage),
                          static_cast<uint16_t>(ttfPageCount), ttfCurrentCharStart, ttfGeneration);
}

void EpubReaderActivity::finishTtfPageRender() {
  // Book profiling trace: confirm the normal AA page turn performs no extra
  // application-level refresh after cleanup (the SDK's cleanup is RAM-only).
  const bool grayExtraDisplay = overlay != Overlay::None && usesToolbarMenu();
  LOG_DBG("GRS", "finishTtfPageRender: extraDisplay=%d", grayExtraDisplay);
  // P4: a chrome popup painted this frame must keep page N's context, so the
  // prerender stays unscheduled for this pass (the popup repaints every
  // render until its duration elapses, then the prerender resumes).
  ttfChromePopupShown = showBookmarkMessage || showDictionaryMessage;
  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }
  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, dictionaryMessageTtf ? tr(STR_DICT_TTF_UNSUPPORTED) : tr(STR_DICT_NO_DICT_SET));
  }
  // P4 one-page-ahead prerender: schedule only for fully clean frames.
  if (!ttfChromePopupShown) ttfSchedulePreRender();
  if (overlay != Overlay::None && usesToolbarMenu()) {
    // The page just re-rendered under the overlay: refresh the snapshot that
    // backs panel->toolbar restores (any previous copy is stale).
    LOG_DBG("GRS", "ttf finish: overlay FAST after gray cleanup overlay=%d", static_cast<int>(overlay));
    if (renderer.hasFrameBuffer()) {
      if (overlayPageStored) {
        renderer.discardStoredBwBuffer();
        overlayPageStored = false;
      }
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
    if (overlayPopup.isActive()) overlayPopup.render(renderer);
    pushOverlayRefresh();
  }
}

void EpubReaderActivity::ttfInvalidateCaches() {
  if (!ttf_) return;
  if (ttfCurrentCharStart > 0) {
    ttfReflowJumpPending = true;
  }
  ttf_->abortSession();
  ttf_->dropPrefetch();
  ttf_->closeChapterCache();
  ttfPrefetchActive = false;
  ttfSpine = -1;
  ttfPage = -1;
  ttfPageCount = 0;
  ttfGenerationValid = false;
  ttfFrameRenderComplete.store(false, std::memory_order_release);
}

void EpubReaderActivity::ttfShowIndexingPopup() {
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  pagesUntilFullRefresh = 1;
}

// Resolves the pending navigation state into a chapter-local target page.
// Returns false when more build output is needed first (jump states stay
// pending and are retried on the next render pass). `needFullBuild` asks for
// a complete build (percent jumps / last-page sentinels), not just enough
// pages to cover the target.
bool EpubReaderActivity::ttfResolveTargetPage(int& targetOut, const freeink::book::LayoutParams& params,
                                              bool& needFullBuild) {
  (void)params;
  const int available = static_cast<int>(ttfPageCount);
  const uint16_t currentSpine = static_cast<uint16_t>(currentSpineIndex);
  const bool buildRunning = ttf_->sessionFor(currentSpine) && ttf_->sessionActive();
  const bool cacheMatchesGeneration = ttf_->cacheReady() && ttf_->cacheGeneration() == ttfGeneration;
  const bool sessionHasTotal =
      ttf_->sessionFor(currentSpine) && ttf_->sessionDone() && ttf_->sessionMatchesGeneration(ttfGeneration);
  const bool completeCache = cacheMatchesGeneration && !ttf_->cachePartial() && !buildRunning;
  const bool haveTotal = sessionHasTotal || completeCache;
  const auto offsetIsAvailable = [this, haveTotal, sessionHasTotal, completeCache](uint32_t offset) {
    if (!haveTotal) return false;
    return sessionHasTotal ? ttf_->sessionTotalChars() > offset : (completeCache && ttf_->cacheTotalChars() > offset);
  };
  const auto canMapCompleteOffset = [&offsetIsAvailable](uint32_t offset) { return offsetIsAvailable(offset); };

  if (pendingPageJump.has_value()) {
    const int jump = *pendingPageJump;
    if (jump == static_cast<int>(std::numeric_limits<uint16_t>::max())) {
      // Last-page sentinel: the end of the CHAPTER, so it may only resolve
      // once the total is known — a suspended partial prefix is not the end
      // (legacy behavior: wait for the full build).
      if (haveTotal) {
        pendingPageJump.reset();
        targetOut = std::max(0, available - 1);
        return true;
      }
      needFullBuild = true;
      return false;
    }
    // A normal jump beyond the built prefix stays pending on a partial
    // cache: consuming it would let a heap-gated background build fall back
    // to the old page. A complete cache clamps it below.
    if (!haveTotal && jump >= available) {
      needFullBuild = true;
      return false;
    }
    pendingPageJump.reset();
    targetOut = std::max(0, jump);
    return true;
  }

  if (ttfRestoreLastPage) {
    // Back from page 0 into the previous chapter (§3.5): last built page,
    // degrading to page 0 on a cold chapter. A partial prefix cannot report
    // the chapter's last page, so wait for the build to complete first.
    if (!haveTotal) {
      needFullBuild = true;
      return false;
    }
    ttfRestoreLastPage = false;
    targetOut = available > 0 ? available - 1 : 0;
    return true;
  }

  if (!pendingAnchor.empty()) {
    const uint32_t anchorHash = freeink::book::ZipCatalog::hashPath(pendingAnchor.c_str());
    uint32_t charOffset = 0;
    uint32_t page = 0;
    if (ttf_->charForAnchor(currentSpine, anchorHash, &charOffset) && canMapCompleteOffset(charOffset) &&
        ttf_->pageForChar(currentSpine, charOffset, &page)) {
      pendingAnchor.clear();
      targetOut = static_cast<int>(page);
      return true;
    }
    if (completeCache) {
      pendingAnchor.clear();
      LOG_DBG("ERS", "Anchor not found in built TTF chapter, opening at page 0");
      targetOut = 0;
      return true;
    }
    needFullBuild = true;
    return false;
  }

  if (ttfReflowJumpPending) {
    // Settings/orientation reflow: restore the position through the character
    // offset of the page that was shown before the caches were dropped. The
    // offset only maps once the chapter's page index is COMPLETE — a partial
    // prefix clamps beyond-watermark offsets and would lose the position.
    uint32_t page = 0;
    if (canMapCompleteOffset(ttfCurrentCharStart) && ttf_->pageForChar(currentSpine, ttfCurrentCharStart, &page)) {
      if (haveTotal) {
        ttfReflowJumpPending = false;
        targetOut = static_cast<int>(page);
        return true;
      }
      needFullBuild = true;
      return false;
    }
    if (completeCache) {
      ttfReflowJumpPending = false;
      targetOut = 0;
      return true;
    }
    needFullBuild = true;
    return false;
  }

  if (ttfHasSavedPosition) {
    if (currentSpineIndex != ttfSavedSpine || ttfSavedGeneration != ttfGeneration) {
      ttfHasSavedPosition = false;
      targetOut = 0;  // generation/spine mismatch: chapter-start degrade (§7)
      return true;
    }
    // Page-anchored restore: a generation-tagged record with charOffset 0
    // carries its position in the record's page number (the KOReader
    // remote-accept save has no TTF char anchor for the remote position; a
    // genuine chapter-start save of page 0 is indistinguishable and identical).
    if (ttfSavedCharOffset == 0) {
      if (haveTotal) {
        ttfHasSavedPosition = false;
        // A complete-but-empty chapter has no pages; clamp(x, 0, -1) would
        // be UB, so degrade to page 0 instead.
        targetOut = available > 0 ? std::clamp(static_cast<int>(nextPageNumber), 0, available - 1) : 0;
        return true;
      }
      needFullBuild = true;
      return false;
    }
    // Generation still matches: wait for a cache that can actually map the
    // offset. pageForChar() clamps beyond-watermark offsets on a partial
    // prefix, so a provisional (prefix) result must not consume the saved
    // position — accept it only under haveTotal.
    uint32_t page = 0;
    if (canMapCompleteOffset(ttfSavedCharOffset) && ttf_->pageForChar(currentSpine, ttfSavedCharOffset, &page)) {
      if (haveTotal) {
        ttfHasSavedPosition = false;
        targetOut = static_cast<int>(page);
        return true;
      }
      needFullBuild = true;
      return false;
    }
    if (haveTotal) {
      ttfHasSavedPosition = false;
      targetOut = 0;  // offset absent from the complete chapter text
      return true;
    }
    needFullBuild = true;
    return false;
  }

  if (pendingOffsetJump.has_value()) {
    // KOReader-sync offset jump: under TTF it addresses the chapter char
    // offset space. A cold/partial cache cannot map it faithfully — keep the
    // jump pending and let the full build produce the complete index first.
    uint32_t page = 0;
    if (canMapCompleteOffset(*pendingOffsetJump) && ttf_->pageForChar(currentSpine, *pendingOffsetJump, &page)) {
      if (haveTotal) {
        pendingOffsetJump.reset();
        targetOut = static_cast<int>(page);
        return true;
      }
      needFullBuild = true;
      return false;
    }
    if (haveTotal) {
      pendingOffsetJump.reset();
      targetOut = 0;  // offset absent from the chapter text
      return true;
    }
    needFullBuild = true;
    return false;
  }

  if (pendingPercentJump) {
    if (!haveTotal || available <= 0) {
      needFullBuild = true;
      return false;
    }
    int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(available));
    if (newPage >= available) newPage = available - 1;
    pendingPercentJump = false;
    targetOut = std::max(0, newPage);
    return true;
  }

  // Default: preserved page (re-render) or the pending default page set by
  // navigation (pageTurn/menu/KOReader).
  targetOut = ttfPage >= 0 ? ttfPage : std::max(0, nextPageNumber);
  return true;
}

void EpubReaderActivity::renderBookTtf() {
  if (!epub || !ttf_) return;
  // Runs under the render task's RenderLock; catches every requestUpdate()
  // exit from the overlay while its deferred chrome refresh is still pending.
  settleOverlayRefresh();
  // P4 prerender: capture and clear the pass flags before any state checks.
  // - prerender pass:      paint the NEXT page's content only (no status bar,
  //                        no flush) into the framebuffer.
  // - fast-display pass:   the framebuffer already holds the next page's
  //                        content; skip the base paint, chrome + commit only.
  // - anything else:       a normal render — navigation, settings, popups,
  //                        rebuilds — and any stale prerender dies here.
  const bool isPreRenderPass = ttfPendingPreRender;
  const bool isBufferDisplayPass = ttfUsePreRenderedBuffer;
  ttfPendingPreRender = false;
  ttfUsePreRenderedBuffer = false;
  if (!isPreRenderPass && !isBufferDisplayPass) ttfInvalidatePreRender();
  ttfChromePopupShown = false;
  // Any render attempt makes the previous framebuffer state provisional: only
  // a successful page+status render below may restore the fast-open flag.
  ttfFrameRenderComplete.store(false, std::memory_order_release);

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };
  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return;  // end of book — isAtEndOfBook() drives the end-of-book menu
  }

  // Render-path heap gate (§3.5): a low DRAM floor postpones the raster to
  // the next loop pass rather than fragmenting the hot path.
  if (ESP.getFreeHeap() < RENDER_MIN_FREE_HEAP) {
    requestUpdate();
    return;
  }

  // 1) Layout params + generation for the current geometry/settings state.
  freeink::book::LayoutParams params;
  ttf_->makeLayoutParams(renderer, params, automaticPageTurnActive);
  if (params.font == nullptr) {
    showBuildError();
    return;
  }
  const uint32_t generation = freeink::book::layoutGenerationHash(params, freeink::book::fontLoader.fontFingerprint());
  ttfGeneration = generation;
  ttfGenerationValid = true;
  // Index-ahead worker: spawn/notify under the current generation + chapter.
  updateFibpWorker(generation, params);

  // 1b) P4 prerender passes. Both re-derive their target from live state (the
  // flags only say a pass was requested — navigation since the schedule shows
  // up here as a different spine/page) and both run with the panel quiet:
  // settleOverlayRefresh() above drained any deferred overlay waveform, and
  // the previous page's commit waited for its own refresh.
  if (isPreRenderPass) {
    ttfRunPreRenderPass(params);
    return;
  }
  if (isBufferDisplayPass) {
    if (ttfFastDisplayPass(params)) return;
    // Page read failed or an image page slipped through: the framebuffer no
    // longer matches the reading position — full render below.
    ttfInvalidatePreRender();
  }

  // 2) Chapter transition. A running session for the chapter we enter (a
  // prefetch build) keeps laying out; anything else aborts (partial commit).
  if (ttfSpine != currentSpineIndex) {
    ttfPrefetchActive = false;
    fibpDeferred_ = false;
    ttf_->dropPrefetch();
    ttfFrameRenderComplete.store(false, std::memory_order_release);
    if (ttfHasSavedPosition && currentSpineIndex != ttfSavedSpine) {
      ttfHasSavedPosition = false;
    }
    if (!ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) {
      ttf_->abortSession();
    }
    ttf_->closeChapterCache();
    ttfSpine = currentSpineIndex;
    ttfPage = -1;
    ttfPageCount = 0;
    if (!ttfReflowJumpPending) {
      // A settings/orientation reflow still needs the displayed page's char
      // anchor — dropping it here would resolve the reflow to offset 0.
      ttfCurrentCharStart = 0;
    }
    const freeink::book::BookStatus st = ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), generation);
    if (st == freeink::book::BookStatus::Stale) {
      LOG_DBG("ERS", "Stale TTF cache for spine %d — rebuilding", currentSpineIndex);
    } else if (st == freeink::book::BookStatus::Ok && ttf_->cachePartial()) {
      LOG_DBG("ERS", "Partial TTF cache (%u pages) — resuming build",
              ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex)));
    }
  }

  ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));

  // 3) Single-writer handoff (R4 optional polish): while the worker builds
  // the chapter we are on, wait for its commit instead of racing a second
  // writer onto the same FIBP file; once it lands, reopen the cache so the
  // resolve/build decisions below see the worker's page count — no sync
  // rebuild, no popup stall beyond the wait.
  if (fibpWorker_ != nullptr && fibpWorker_->active() &&
      fibpWorker_->buildingSpine() == static_cast<uint16_t>(currentSpineIndex)) {
    if (!fibpDeferred_) {
      fibpDeferred_ = true;
      fibpDeferStartMs_ = millis();
      LOG_DBG("ERS", "Chapter %d build delegated to prefetch worker", currentSpineIndex);
      ttfShowIndexingPopup();
    }
    // The worker owns this build; re-polling with full renders every loop
    // tick flip-flops the power governor and burns the CPU. Re-check at a
    // slow cadence — the commit pickup below runs on the next poll.
    if (millis() - fibpDeferPollMs_ >= fibpDeferPollMs) {
      fibpDeferPollMs_ = millis();
      requestUpdate();
    }
    // A long delegated build parks the user on the Indexing popup for the
    // whole spine (the plan builds the entered chapter last, and a claim
    // that landed right before the page turn makes the wait the full build).
    // After a short grace period the reader takes the spine over: stop the
    // worker (it aborts at the next page boundary), then the normal path
    // below resumes the worker's partial and builds only up to the target
    // page at full speed. updateFibpWorker() respawns the worker once the
    // session drains; it skips this spine (partial/complete now exists).
    if (millis() - fibpDeferStartMs_ >= fibpDeferTakeoverMs) {
      LOG_DBG("ERS", "Delegated build too slow — taking over spine %d", currentSpineIndex);
      stopFibpWorker();
      fibpDeferred_ = false;
      requestUpdate();
    }
    return;
  }
  if (fibpDeferred_) {
    // The delegated build finished (or failed): pick up the committed cache.
    fibpDeferred_ = false;
    ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), generation);
    ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
  }

  // 4) Resolve the target page (jump states may need more build first).
  bool needFullBuild = false;
  int target = -1;
  const bool resolved = ttfResolveTargetPage(target, params, needFullBuild);

  // 4) Build toward the target, synchronously like the legacy path. The heap
  // gate can defer the remainder to the background ticks.
  bool wasBuilding = false;
  if ((needFullBuild || (resolved && target >= static_cast<int>(ttfPageCount))) &&
      !ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) {
    const uint32_t spineBytes = ttf_->catalog().spineSize(static_cast<size_t>(currentSpineIndex));
    // Indexing popup mirrors the legacy engine: a cold or stale cache and a
    // settings-driven reflow always rebuild across render passes, so show it;
    // a warm top-up of a partial cache stays threshold-gated to avoid a flash.
    const bool partialCache = ttf_->cacheReady() && ttf_->cachePartial();
    if (!ttf_->cacheReady() || ttfReflowJumpPending || needFullBuild ||
        (partialCache && (spineBytes > BUILD_POPUP_BYTE_THRESHOLD || target > BUILD_POPUP_PAGE_THRESHOLD))) {
      ttfShowIndexingPopup();
    }
    const freeink::book::BookStatus st =
        freeink::book::ensureChapterSession(*ttf_, static_cast<uint16_t>(currentSpineIndex), params, generation);
    if (st != freeink::book::BookStatus::Ok) {
      LOG_ERR("ERS", "TTF session begin failed: %s", bookStatusName(st));
      showBuildError();
      return;
    }
    wasBuilding = true;
  }
  wasBuilding = wasBuilding || ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex));

  if (ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) {
    // Build one page chunk per render pass on a warm chapter; allow a short
    // cold-open burst so an empty chapter reaches a readable page faster.
    // Further chunks are driven by the next render/background tick so long
    // jumps still cannot freeze input.
    constexpr uint8_t kWarmSyncBuildChunks = 1;
    constexpr uint8_t kColdStartSyncBuildChunks = 2;
    const uint8_t chunksPerPass = ttfPageCount == 0 ? kColdStartSyncBuildChunks : kWarmSyncBuildChunks;
    uint8_t chunksThisPass = 0;
    while (ttf_->sessionActive() &&
           (needFullBuild ? true
                          : (resolved && target >= static_cast<int>(ttf_->availablePageCount(
                                                       static_cast<uint16_t>(currentSpineIndex)))))) {
      if (!buildTickHeapGate()) break;
      if (chunksThisPass++ >= chunksPerPass) {
        if (ttf_->sessionActive()) {
          ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
          requestUpdate();
          return;
        }
        break;
      }
      freeink::book::BookStatus st = freeink::book::BookStatus::Ok;
      if (freeink::book::pumpChapterChunk(*ttf_, static_cast<uint16_t>(currentSpineIndex), BUILD_PAGES_PER_CHUNK,
                                          &st) == freeink::book::ChapterPump::Failed) {
        LOG_ERR("ERS", "TTF build failed: %s", bookStatusName(st));
        showBuildError();
        return;
      }
    }
    ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
  }
  if (wasBuilding && !ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) {
    // The build finished (or aborted) during this pass — reopen the cache so
    // serving switches from the writer index to the committed file.
    ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), generation);
    ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
  }

  if (!resolved) {
    // Target not derivable yet (full build in flight). Background ticks keep
    // going; jump states stay pending for the next pass.
    requestUpdate();
    return;
  }

  if (ttfPageCount == 0) {
    LOG_DBG("ERS", "TTF: no pages in chapter");
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    nextPageNumber = 0;
    cachedChapterTotalPageCount = 0;
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (target < 0) target = ttfPage >= 0 ? ttfPage : 0;
  if (target >= static_cast<int>(ttfPageCount)) {
    if (ttf_->sessionActive()) {
      requestUpdate();  // build will cover it
      return;
    }
    target = static_cast<int>(ttfPageCount) - 1;
  }
  ttfPage = target;

  updateBookmarkFlag();

  // 5) Read + rasterize the page. Run text lives in the runtime's scratch
  // arena for exactly this block. A failed read must not expose the prior
  // frame through overlay fast paths.
  ttfFrameRenderComplete.store(false, std::memory_order_release);
  const size_t scratchMark = ttf_->scratch().mark();
  freeink::book::Page page{};
  if (!ttf_->readPage(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(ttfPage), &page)) {
    ttf_->scratch().release(scratchMark);
    LOG_ERR("ERS", "TTF page read failed (spine %d page %d)", currentSpineIndex, ttfPage);
    requestUpdate();  // transient SD failure; retry on the next pass
    return;
  }
  ttfCurrentCharStart = page.charStart;

  // §3.5 item 8: footnote list from the engine's PageLink substrate. Internal
  // (resolvable) targets only — external URLs never enter the reader flow.
  ttfExtractFootnotes(page);

  renderer.clearScreen(0xFF);
  paintTtfPage(page, params.font);
#ifdef READING_STATS_ENABLED
  // Engine-computed word count (ChapterLayout counts over the paragraph text
  // before run segmentation; justified runs carry no spaces — counting run
  // text collapsed a justified page to ~2 words).
  currentPageWordsOnPage = page.wordCount;
#endif

  // 6) Chrome after the page: keep the legacy position mirrors in sync so
  // renderStatusBar/KOReader/bookmark code reads the same values.
  nextPageNumber = ttfPage;
  cachedChapterTotalPageCount = static_cast<int>(ttfPageCount);
  renderStatusBar();
  // Do not mark the frame complete yet: the TTF gray/image-specific passes
  // below may still be modifying the display planes.
  ttfCommitFrame(page, params, scratchMark);
}

// Display commit + finalize tail shared by the normal render and the P4 fast
// turn (the framebuffer already holds the painted content in both cases —
// the fast pass skips only the base paint, not the commit).
void EpubReaderActivity::ttfCommitFrame(const freeink::book::Page& page, const freeink::book::LayoutParams& params,
                                        size_t scratchMark) {
  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };
  // Same §11 Q7 predicate paintTtfPage used for the base pass above: images
  // keep the 1bpp engine path (no plane bits), so the dual-plane block runs
  // only for text-only AA pages on a panel whose controller supports the
  // 4-level gray mode at all.
  const bool pageHasImages = page.imageCount > 0 && SETTINGS.imageRendering == CrossPointSettings::IMAGES_DISPLAY;
  const auto grayCaps = renderer.grayscaleCapabilities();
  // Degrade-aware: a build without the mono module paints Smooth planes even
  // when the setting asks for Crisp (the loader degraded the faces).
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  const bool smoothText = !freeink::book::fontLoader.effectiveMonochrome();
#else
  const bool smoothText = SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_SMOOTH;
#endif
  const bool grayParity = smoothText && !pageHasImages && grayCaps.supported();
  LOG_DBG("GRS", "ttfGrayPath: aa=%d images=%d strip=%d cadence=%d/%d", grayParity, pageHasImages,
          grayCaps.stripUploads, pagesUntilFullRefresh, SETTINGS.getRefreshFrequency());
  if (grayParity) {
    // §11 Q7 construction (a): dual-plane gray parity. Base refresh ordering
    // mirrors the legacy AA path: cleanup cycle when due, otherwise the
    // grayscale base waveform; then the plane walks, the gray display, and
    // the baseline cleanup. Transport follows the panel: strips where the
    // driver supports them, full-frame plane buffers otherwise (UC8279 X4
    // advertises Overlay gray with stripUploads=false — the full planes go
    // through the same driver's full-plane upload, not a strip flag flip).
    LOG_DBG("GRS", "ttf gray route: transport=%s pagesUntilFullRefresh=%d refreshFrequency=%d images=%d",
            grayCaps.stripUploads ? "strips" : "full-frame", pagesUntilFullRefresh, SETTINGS.getRefreshFrequency(),
            pageHasImages);
    if (grayCaps.stripUploads) {
      renderTtfGrayStrips(page, params, scratchMark);
    } else {
      renderTtfGrayFullFrame(page, params, scratchMark);
    }
    // The page's run text lives in the scratch arena: release only after
    // the last plane pass has walked it.
    ttf_->scratch().release(scratchMark);
    lastRenderCompleteMs = millis();
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
    LOG_DBG("REND", "reader gray render stack high-water=%u bytes",
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    memSentinelCheck("reader gray render");
#endif
    // Cleanup has restored the framebuffer after the final gray-plane pass.
    ttfFrameRenderComplete.store(true, std::memory_order_release);
#ifdef READING_STATS_ENABLED
    pageShownAtMs = millis();
#endif
    ttfSaveProgress();
    showPendingSyncSaveError();
    finishTtfPageRender();
    return;
  }

  // 1bpp path: the page's run text was last touched by paintTtfPage above.
  ttf_->scratch().release(scratchMark);

  const bool canAsyncDisplay = renderer.supportsAsyncRefresh();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, canAsyncDisplay);
  if (canAsyncDisplay) {
    // E-ink refresh is running: overlap background work with it (legacy
    // renderContents pattern).
    prefetchNextChapterDuringDisplay();
    if (ttf_->sessionActive() && buildTickHeapGate()) {
      // Route through the tick so session completion reopens the committed
      // cache (a raw stepBuild would leave the reader without a source).
      ttfBackgroundBuildTick();
    }
    renderer.waitRefreshComplete();
  }
  lastRenderCompleteMs = millis();
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  // In-render stack probe: the post-render REND telemetry never ran for the
  // crashing reader renders — measure the reader path's own high-water HERE,
  // before render() returns (the deepest path: rebuild + FT paint + chrome).
  LOG_DBG("REND", "reader render stack high-water=%u bytes",
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  // Heap/PSRAM budget across book renders (moved here from the generic render
  // wrapper): catches FT-side leaks (faces, glyph backing) and DRAM pressure.
  LOG_DBG("REND", "render mem: HeapFree=%u HeapMin=%u MaxAlloc=%u PSRAMFree=%u PSRAMMin=%u",
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(ESP.getFreePsram()),
          static_cast<unsigned>(ESP.getMinFreePsram()));
  memSentinelCheck("reader bw render");
#endif
  // The B/W frame and status chrome are fully painted (and any async submit
  // has completed) before overlay opens may paint directly onto this frame.
  ttfFrameRenderComplete.store(true, std::memory_order_release);
#ifdef READING_STATS_ENABLED
  pageShownAtMs = millis();
#endif

  ttfSaveProgress();
  showPendingSyncSaveError();
  finishTtfPageRender();
}

void EpubReaderActivity::ttfInvalidatePreRender() {
  ttfPreRendered.ready = false;
  ttfPreRendered.spineIndex = -1;
  ttfPreRendered.pageIndex = -1;
}

// Schedule the one-page-ahead prerender after a fully committed frame (see
// the header contract). Refuses to schedule while the panel could expose the
// prerendered content to the glass before the reading position moves there:
// an open overlay (chrome over the page), a pending deferred overlay push,
// or a chrome popup painted this pass (its context is page N).
void EpubReaderActivity::ttfSchedulePreRender() {
  if (ttfPendingPreRender || ttfPreRendered.ready) return;
  if (ttfChromePopupShown || overlay != Overlay::None) return;
  if (overlayRefreshPending.load(std::memory_order_acquire)) return;
  if (!ttf_ || ttfSpine != currentSpineIndex || !ttf_->cacheReady()) return;
  // A build in flight owns the FIBP writer state and re-renders anyway —
  // prerendering into that churn is wasted work (and it must not fight the
  // chapter prefetch for the same pages).
  if (ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) return;
  if (ttfPage < 0 || ttfPage + 1 >= static_cast<int>(ttfPageCount)) return;  // chapter end: prefetch owns it
  if (ESP.getFreeHeap() < RENDER_MIN_FREE_HEAP) return;
  ttfPendingPreRender = true;
  requestUpdate();
}

// P4 prerender pass: paint the NEXT page's content-only into the framebuffer
// (no status bar, no flush) while the glass still shows page N. One
// RenderLock-protected transaction; the previous pass's commit already
// waited for the refresh (waitRefreshComplete in ttfCommitFrame), so the
// paint cannot expose a partial frame.
void EpubReaderActivity::ttfRunPreRenderPass(const freeink::book::LayoutParams& params) {
  // Re-derive the target from live state — the schedule flag alone must
  // never paint a page the reading position has left.
  if (!ttf_ || ttfSpine != currentSpineIndex || !ttf_->cacheReady()) return;
  if (ttfPreRendered.ready) return;
  const int nextPage = ttfPage + 1;
  if (ttfPage < 0 || nextPage >= static_cast<int>(ttfPageCount)) return;
  if (ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) return;
  if (overlay != Overlay::None || overlayRefreshPending.load(std::memory_order_acquire)) return;
  if (ESP.getFreeHeap() < RENDER_MIN_FREE_HEAP) return;

  const size_t scratchMark = ttf_->scratch().mark();
  freeink::book::Page page{};
  if (!ttf_->readPage(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(nextPage), &page)) {
    ttf_->scratch().release(scratchMark);
    LOG_DBG("ERS", "TTF prerender: page %d read failed", nextPage);
    return;
  }
  if (page.imageCount > 0 && SETTINGS.imageRendering == CrossPointSettings::IMAGES_DISPLAY) {
    // Image pages keep the 1bpp engine path with image decode — excluded
    // from prerendering (the same exclusion upstream made).
    ttf_->scratch().release(scratchMark);
    return;
  }
  renderer.clearScreen(0xFF);
  paintTtfPage(page, params.font);
  ttf_->scratch().release(scratchMark);
  // Publish only after the paint, metadata first so a reader of ready==true
  // never sees stale indices.
  ttfPreRendered.spineIndex = static_cast<int16_t>(currentSpineIndex);
  ttfPreRendered.pageIndex = static_cast<int16_t>(nextPage);
  ttfPreRendered.ready = true;
  // The framebuffer no longer holds a clean displayed page: overlay opens
  // must not snapshot it (their hasRenderedPage gate reads this flag).
  ttfFrameRenderComplete.store(false, std::memory_order_release);
  LOG_DBG("ERS", "TTF prerendered page %d/%d", nextPage, ttfPageCount - 1);
}

// P4 fast turn: the framebuffer holds the prerendered content of the page
// the reading position just moved to. Skip the base paint (layout/build/
// readPage-clearScreen/paintTtfPage), load the page for the mirrors and the
// gray plane walks, draw the status bar, and commit. Returns false when any
// precondition fails — the caller falls through to a full render.
bool EpubReaderActivity::ttfFastDisplayPass(const freeink::book::LayoutParams& params) {
  if (!ttf_ || !ttfPreRendered.ready) return false;
  if (ttfPreRendered.spineIndex != currentSpineIndex || ttfPreRendered.pageIndex != ttfPage) return false;
  if (ttfSpine != currentSpineIndex || ttfPage < 0 || ttfPage >= static_cast<int>(ttfPageCount)) return false;

  const size_t scratchMark = ttf_->scratch().mark();
  freeink::book::Page page{};
  if (!ttf_->readPage(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(ttfPage), &page)) {
    ttf_->scratch().release(scratchMark);
    LOG_ERR("ERS", "TTF fast turn: page read failed (spine %d page %d)", currentSpineIndex, ttfPage);
    return false;
  }
  ttfCurrentCharStart = page.charStart;
  if (page.imageCount > 0 && SETTINGS.imageRendering == CrossPointSettings::IMAGES_DISPLAY) {
    // Image pages are never prerendered; a stale flag must not fast-path them.
    ttf_->scratch().release(scratchMark);
    return false;
  }
  ttfExtractFootnotes(page);

  // Content is already in the framebuffer — chrome only, then commit.
  nextPageNumber = ttfPage;
  cachedChapterTotalPageCount = static_cast<int>(ttfPageCount);
  renderStatusBar();
#ifdef READING_STATS_ENABLED
  currentPageWordsOnPage = page.wordCount;
#endif
  LOG_DBG("GRS", "ttf fast turn: page %d/%d displayed from prerender", ttfPage, ttfPageCount - 1);
  ttfCommitFrame(page, params, scratchMark);
  return true;
}

void EpubReaderActivity::ttfExtractFootnotes(const freeink::book::Page& page) {
  // The number label is the superscript run overlapping the link rect,
  // falling back to the link's ordinal position.
  currentPageFootnotes.clear();
  currentPageFootnotes.reserve(page.linkCount);
  for (uint16_t l = 0; l < page.linkCount; ++l) {
    const auto& link = page.links[l];
    // SDK contract fills both strings (dropped on alloc failure upstream);
    // guard anyway so future SDK drift cannot null-deref here.
    if (link.target == nullptr) continue;
    std::string href = link.target;
    if (link.fragment != nullptr && link.fragment[0] != '\0') href += std::string("#") + link.fragment;
    if (href.empty() || href.rfind("http", 0) == 0) continue;
    if (!epub || (link.target[0] != '\0' && epub->resolveHrefToSpineIndex(href) < 0)) continue;

    FootnoteEntry entry;
    std::string number;
    for (uint16_t r = 0; r < page.runCount; ++r) {
      const auto& run = page.runs[r];
      // Superscript marker runs are short; the label must sit inside the
      // link's rect to be the marker for THIS link.
      if (run.len == 0 || run.len > 4 || (run.styleFlags & freeink::book::StyleSuperscript) == 0) continue;
      const bool withinY = run.baselineY >= link.y && run.baselineY <= link.y + static_cast<int32_t>(link.height);
      const bool withinX = run.x >= link.x && run.x < link.x + static_cast<int32_t>(link.width);
      if (withinY && withinX) {
        number.assign(run.text, run.len);
        break;
      }
    }
    if (number.empty()) {
      char ordinal[8];
      snprintf(ordinal, sizeof(ordinal), "%u", static_cast<unsigned>(l + 1));
      number = ordinal;
    }
    strncpy(entry.number, number.c_str(), FOOTNOTE_NUMBER_LEN - 1);
    entry.number[FOOTNOTE_NUMBER_LEN - 1] = '\0';
    strncpy(entry.href, href.c_str(), FOOTNOTE_HREF_LEN - 1);
    entry.href[FOOTNOTE_HREF_LEN - 1] = '\0';
    currentPageFootnotes.push_back(entry);
  }
}

void EpubReaderActivity::ttfDisplayGrayBase() {
  // GRS: base-entry diagnostic — request mode, cadence state, and driver state flags.
  // The overlay base path is used for all TTF text AA pages: the owner's research
  // recommends Fast base + sparse overlay masks + stock short AA waveform. Direct
  // combined (absolute planes + quality bank) is reserved for full-screen images
  // and sleep covers; selecting it for text pages causes the jarring quality
  // waveform flash the research describes.
  //
  // Cadence knob: SETTINGS.refreshFrequency controls how many consecutive AA
  // pages run before the deliberate Half scrub. The first AA page and any
  // driver-invalid state still take a real B/W activation.
  if (pagesUntilFullRefresh <= 1) {
    LOG_DBG("GRS", "ttfDisplayGrayBase: cadence=full (pages<=1) requesting HALF+precondition");
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    renderer.preconditionGrayscale();
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    return;
  }
  LOG_DBG("GRS", "ttfDisplayGrayBase: cadence=fast pagesUntilFullRefresh=%d requesting overlay FAST base",
          pagesUntilFullRefresh);
  // Overlay mode: uses Fast base transition when the driver state allows
  // (_grayRefreshedOnce && _oldPlaneValid && !_needFullClear), otherwise falls
  // back to a real B/W display(). Always uses overlay masks — never Direct
  // combined for text pages (see SDK Uc8279X4Driver::displayGrayscaleBase).
  renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  pagesUntilFullRefresh--;
}

void EpubReaderActivity::renderTtfGrayStrips(const freeink::book::Page& page, const freeink::book::LayoutParams& params,
                                             size_t scratchMark) {
  (void)scratchMark;  // released by the caller after this walk
#ifdef BOOK_PROFILE
  const unsigned long renderStartMs = millis();
#endif
  constexpr int STRIP_ROWS = 80;
  const int gh = renderer.getDisplayHeight();
  const int gwBytes = renderer.getDisplayWidthBytes();
  // Allocate both plane bands before mutating refresh state: if either
  // allocation fails, the B/W fallback runs from untouched cadence state.
  auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
  auto msbScratch = scratch ? makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS) : nullptr;
  if (!scratch || !msbScratch) {
    LOG_ERR("ERS", "OOM: TTF plane bands (%d bytes); displaying B/W page", gwBytes * STRIP_ROWS);
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, /*async=*/false);
    return;
  }

  ttfDisplayGrayBase();
#ifdef BOOK_PROFILE
  const unsigned long tBaseMs = millis();
#endif
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_DUAL);
  for (int y = 0; y < gh; y += STRIP_ROWS) {
    const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
    renderer.beginStripTarget(scratch.get(), y, rows, msbScratch.get());
    renderer.clearScreen(0x00);
    freeink::book::PagePaint::paintPlanes(page, *static_cast<freeink::book::FontChain*>(params.font), renderer);
    renderer.endStripTarget();
    renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
    renderer.writeGrayscalePlaneStrip(false, msbScratch.get(), y, rows);
  }
#ifdef BOOK_PROFILE
  const unsigned long tPlanesMs = millis();
#endif
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
#ifdef BOOK_PROFILE
  const unsigned long tGrayMs = millis();
#endif
  renderer.cleanupGrayscaleWithFrameBuffer();
#ifdef BOOK_PROFILE
  LOG_DBG("PROF", "phase=ttf_gray transport=strips bands=%d base=%lums planes=%lums gray=%lums cleanup=%lums",
          (gh + STRIP_ROWS - 1) / STRIP_ROWS, tBaseMs - renderStartMs, tPlanesMs - tBaseMs, tGrayMs - tPlanesMs,
          millis() - tGrayMs);
#endif
}

void EpubReaderActivity::renderTtfGrayFullFrame(const freeink::book::Page& page,
                                                const freeink::book::LayoutParams& params, size_t scratchMark) {
  // Full-frame fallback for non-strip panels (UC8279 X4: Overlay gray with
  // stripUploads=false). The dual-plane walk targets two complete plane
  // buffers — the same bits the strip path produces, one DUAL walk instead
  // of bands — submitted through the driver's full-plane upload. The panel's
  // advertised restrictions are preserved: no strip flag is flipped.
  (void)scratchMark;  // released by the caller after this walk
#ifdef BOOK_PROFILE
  const unsigned long renderStartMs = millis();
#endif
  const int gh = renderer.getDisplayHeight();
  const size_t planeBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * static_cast<size_t>(gh);
  // Size guard before the pool allocation (CWE-400 discipline); panel
  // geometry bounds this (800x480 needs 48000/plane), the check keeps a
  // future panel change honest.
  constexpr size_t kMaxFullFrameGrayBytes = 128 * 1024;
#if defined(BOARD_HAS_PSRAM)
  // PSRAM builds: two 48KB planes are trivial for the 8MB pool.
  const bool planesFit = planeBytes > 0 && planeBytes <= kMaxFullFrameGrayBytes;
#else
  // DRAM tier: the legacy reader's nontiled-dual heap gate.
  const bool planesFit = planeBytes > 0 && planeBytes <= kMaxFullFrameGrayBytes &&
                         ESP.getFreeHeap() >= planeBytes + 60000 && ESP.getMaxAllocHeap() >= planeBytes + 16 * 1024;
#endif
  auto lsbPlane = planesFit ? poolMakeBytes(planeBytes) : PoolBytes{};
  auto msbPlane = lsbPlane ? poolMakeBytes(planeBytes) : PoolBytes{};
  if (!lsbPlane || !msbPlane) {
    LOG_ERR("ERS", "OOM: TTF full-frame gray planes (%u bytes); displaying B/W page", (unsigned)planeBytes);
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, /*async=*/false);
    return;
  }

  ttfDisplayGrayBase();
#ifdef BOOK_PROFILE
  const unsigned long tBaseMs = millis();
#endif
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_DUAL);
  // One "full-frame strip" (origin 0, panel rows): drawGrayDualPixel lands
  // each tone's plane bits in the two private buffers, orientation-aware.
  renderer.beginStripTarget(lsbPlane.get(), 0, gh, msbPlane.get());
  renderer.clearScreen(0x00);
  freeink::book::PagePaint::paintPlanes(page, *static_cast<freeink::book::FontChain*>(params.font), renderer);
  renderer.endStripTarget();
#ifdef BOOK_PROFILE
  const unsigned long tPlanesMs = millis();
#endif
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.copyGrayscaleLsbBuffers(lsbPlane.get());
  renderer.copyGrayscaleMsbBuffers(msbPlane.get());
  renderer.displayGrayBuffer();
#ifdef BOOK_PROFILE
  const unsigned long tGrayMs = millis();
#endif
  renderer.cleanupGrayscaleWithFrameBuffer();
#ifdef BOOK_PROFILE
  LOG_DBG("PROF", "phase=ttf_gray transport=full_frame plane_bytes=%u base=%lums planes=%lums gray=%lums cleanup=%lums",
          (unsigned)planeBytes, tBaseMs - renderStartMs, tPlanesMs - tBaseMs, tGrayMs - tPlanesMs, millis() - tGrayMs);
#endif
}

void EpubReaderActivity::paintTtfPage(const freeink::book::Page& page, void* font) {
  // §11 Q7 construction (a): with text AA engaged the base paints via
  // PagePaint (the tone-1 boundary of the shared uniform quantizer) and a
  // dual plane walk supplies the two gray tones through the panel's AA
  // waveform — the same 4-level pipeline the bitmap reader uses. Images
  // keep the 1bpp engine path (no plane bits for image pixels).
  const bool pageHasImages = page.imageCount > 0 && SETTINGS.imageRendering == CrossPointSettings::IMAGES_DISPLAY;
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  const bool smoothText = !freeink::book::fontLoader.effectiveMonochrome();
#else
  const bool smoothText = SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_SMOOTH;
#endif
  const bool grayParity = smoothText && !pageHasImages && renderer.grayscaleCapabilities().supported();
  auto* chain = static_cast<freeink::book::FontChain*>(font);
  if (grayParity) {
    freeink::book::PagePaint::paintText(page, *chain, renderer);
  } else {
    const freeink::book::FrameTarget frameTarget = makeFrameTarget(renderer);
    freeink::book::PageRenderer::renderText(page, *chain, frameTarget, nullptr);
    // Ruby annotations are engine records — the same pass the engine's own
    // render() runs; no CrossPoint layout involvement.
    if (page.rubyCount > 0) {
      freeink::book::PageRenderer::renderRubies(page, *chain, frameTarget);
    }
  }
  freeink::book::PageRenderer::renderRules(page, makeFrameTarget(renderer));
  if (pageHasImages) {
    const freeink::book::BookStatus st = freeink::book::PageRenderer::renderImages(
        page, ttf_->source(), ttf_->catalog().zip(), ttf_->scratch(), makeFrameTarget(renderer));
    if (st != freeink::book::BookStatus::Ok) {
      LOG_DBG("ERS", "TTF image render failed: %s", bookStatusName(st));
    }
  } else if (SETTINGS.imageRendering == CrossPointSettings::IMAGES_PLACEHOLDER) {
    // §3.5 item 11: image policy is CrossPoint-side; placeholder mode draws
    // the engine's reserved geometry as an outline instead of decoding.
    for (uint16_t m = 0; m < page.imageCount; ++m) {
      const auto& image = page.images[m];
      if (image.width <= 0 || image.height <= 0) continue;
      renderer.drawRect(image.x, image.y, image.width, image.height);
    }
  }
}

void EpubReaderActivity::renderTtfSelectorPage(void* ctx, GfxRenderer& renderer) {
  auto* self = static_cast<EpubReaderActivity*>(ctx);
  if (!self->ttf_) return;
  const size_t scratchMark = self->ttf_->scratch().mark();
  freeink::book::Page page{};
  if (!self->ttf_->readPage(static_cast<uint16_t>(self->currentSpineIndex), static_cast<uint16_t>(self->ttfPage),
                            &page)) {
    self->ttf_->scratch().release(scratchMark);
    LOG_ERR("ERS", "TTF selector page read failed (spine %d page %d)", self->currentSpineIndex, self->ttfPage);
    return;
  }
  freeink::book::LayoutParams params;
  self->ttf_->makeLayoutParams(renderer, params, false);
  if (params.font != nullptr) {
    self->paintTtfPage(page, params.font);
  }
  self->ttf_->scratch().release(scratchMark);
}

void EpubReaderActivity::ttfBackgroundBuildTick() {
  if (!ttf_) return;
  if (ttf_->sessionActive()) {
    const uint16_t spine = ttf_->sessionSpine();
    freeink::book::BookStatus st = freeink::book::BookStatus::Ok;
    if (freeink::book::pumpChapterChunk(*ttf_, spine, BACKGROUND_BUILD_PAGES_PER_TICK, &st) ==
        freeink::book::ChapterPump::Failed) {
      LOG_ERR("ERS", "Background TTF build failed: %s", bookStatusName(st));
      ttfPrefetchActive = false;
      return;
    }
    if (!ttf_->sessionActive()) {  // finished (or failed) in this tick
      if (spine == static_cast<uint16_t>(currentSpineIndex)) {
        ttf_->openChapterCache(spine, ttfGeneration);
        ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
        requestUpdate();
      } else {
        ttfPrefetchActive = false;
        // The committed cache replaced the partial file the prefetch reader
        // describes — drop it so the next tick reopens the complete cache
        // instead of restarting a rebuild from the stale partial.
        ttf_->dropPrefetch();
      }
    }
    return;
  }
  // Partial cache below the read watermark: resume the extension build.
  if (ttf_->cacheReady() && ttf_->cachePartial() && ttfSpine == currentSpineIndex &&
      !ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex)) &&
      ttfPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(ttfPageCount)) {
    freeink::book::LayoutParams params;
    ttf_->makeLayoutParams(renderer, params, automaticPageTurnActive);
    if (params.font == nullptr) return;  // no reader font chain: cannot lay out
    if (freeink::book::ensureChapterSession(*ttf_, static_cast<uint16_t>(currentSpineIndex), params, ttfGeneration) ==
        freeink::book::BookStatus::Ok) {
      freeink::book::BookStatus st = freeink::book::BookStatus::Ok;
      const freeink::book::ChapterPump pump = freeink::book::pumpChapterChunk(
          *ttf_, static_cast<uint16_t>(currentSpineIndex), BACKGROUND_BUILD_PAGES_PER_TICK, &st);
      if (pump == freeink::book::ChapterPump::Complete) {
        ttf_->openChapterCache(static_cast<uint16_t>(currentSpineIndex), ttfGeneration);
      }
      ttfPageCount = ttf_->availablePageCount(static_cast<uint16_t>(currentSpineIndex));
      if (st != freeink::book::BookStatus::Ok) {
        LOG_DBG("ERS", "Partial TTF resume step failed: %s", bookStatusName(st));
      }
    }
  }
}

void EpubReaderActivity::updateFibpWorker(const uint32_t generation, const freeink::book::LayoutParams& params) {
  if (!ttf_ || !epub) return;
  const char* family = SETTINGS.ttfFontFamilyName;
  if (fibpWorker_ != nullptr && fibpBegun_ && strncmp(fibpFamily_, family, sizeof(fibpFamily_)) != 0) {
    // Family swap: the worker's face set must be rebuilt for the new bytes.
    stopFibpWorker();
  }
  if (fibpWorker_ == nullptr) {
    fibpWorker_ = makeUniqueNoThrow<freeink::book::FibpPrefetchWorker>();
    if (fibpWorker_ == nullptr) return;
  }
  if (!fibpBegun_) {
    // Single-writer: a legacy prefetch/sync session in flight owns its spine
    // — wait for it to drain before spawning the worker.
    if (ttf_->sessionActive()) return;
    freeink::book::FibpPrefetchWorker::BeginContext ctx;
    snprintf(ctx.epubPath, sizeof(ctx.epubPath), "%s", bookPath.c_str());
    snprintf(ctx.cacheDir, sizeof(ctx.cacheDir), "%s/ficache", epub->getCachePath().c_str());
    snprintf(ctx.familyName, sizeof(ctx.familyName), "%s", family);
    ctx.spineCount = static_cast<uint16_t>(epub->getSpineItemsCount());
    ctx.generation = generation;
    ctx.layout = params;  // scalar fields used; the worker re-binds pointers
    fibpBegun_ = fibpWorker_->begin(ctx);
    if (fibpBegun_) {
      snprintf(fibpFamily_, sizeof(fibpFamily_), "%s", family);
      fibpNotifiedGen_ = generation;
    }
  } else if (generation != fibpNotifiedGen_) {
    fibpWorker_->notifyGeneration(generation, params);
    fibpNotifiedGen_ = generation;
  }
  if (fibpBegun_) {
    // Raw progress only — the worker applies the 10%-remaining policy. This
    // runs on every reader render (page turns included), so a mid-chapter
    // threshold crossing fires on the turn that crosses it, and a chapter
    // entered already past the threshold triggers immediately. The legacy
    // Section path has no FIBP worker (ttf_ is null there → inert stub), so
    // only the TTF mirrors apply here. ttfPage's -1 chapter-transition
    // sentinel (page not yet laid out) clamps to 0 — casting it raw would
    // read as 65535 and fire the trigger at ~0% progress.
    const uint16_t page = ttfPage >= 0 ? static_cast<uint16_t>(ttfPage) : 0;
    fibpWorker_->notifyChapterProgress(static_cast<uint16_t>(currentSpineIndex), page,
                                       static_cast<uint16_t>(ttfPageCount));
  }
}

void EpubReaderActivity::stopFibpWorker() {
  if (fibpWorker_ == nullptr) return;
  if (fibpWorker_->cancel()) {
    fibpWorker_.reset();
  } else {
    // Wedged join: the worker object owns state a live task still touches —
    // release ownership WITHOUT destroying (logged leak beats use-after-free).
    LOG_ERR("ERS", "FIBP worker did not stop — releasing without destroy");
    (void)fibpWorker_.release();
  }
  fibpBegun_ = false;
  fibpFamily_[0] = '\0';
  fibpDeferred_ = false;
}

void EpubReaderActivity::ttfPrefetchTick() {
  if (!ttf_ || !epub || buildHeapPaused) return;
  // Single-writer: while the FIBP worker feature is active (begun) the worker
  // owns next-chapter prefetching — including its 10%-remaining trigger — so
  // the legacy in-activity build must not index ahead of it; it stays for
  // inert builds only (stub / begin failure).
  if (fibpWorker_ != nullptr && fibpBegun_) return;
  if (ttf_->sessionActive()) return;  // the current chapter's build wins

  const int nextSpine = currentSpineIndex + 1;
  if (nextSpine >= epub->getSpineItemsCount()) return;

  // Warm the next chapter's cache-reader index (cheap; serves nothing else).
  if (ttfGenerationValid) {
    ttf_->openPrefetch(static_cast<uint16_t>(nextSpine), ttfGeneration);
  }

  if (ttf_->sessionFor(static_cast<uint16_t>(nextSpine))) {
    if (ttf_->sessionActive()) {
      freeink::book::pumpChapterChunk(*ttf_, static_cast<uint16_t>(nextSpine), BACKGROUND_BUILD_PAGES_PER_TICK);
    } else {
      ttfPrefetchActive = false;
      ttf_->dropPrefetch();
      ttf_->openPrefetch(static_cast<uint16_t>(nextSpine), ttfGeneration);
    }
    return;
  }

  // Start building the next chapter only while the current one is fully
  // served from its cache AND the next spine has no complete cache yet —
  // otherwise a warm prefetch reader would be rewritten on every idle tick.
  // A suspended partial next-chapter cache still earns a build (§3.5).
  if (!ttfPrefetchActive && !(ttf_->prefetchFor(nextSpine) && !ttf_->prefetchPartial()) && ttf_->cacheReady() &&
      !ttf_->cachePartial() && !ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex))) {
    freeink::book::LayoutParams params;
    ttf_->makeLayoutParams(renderer, params, automaticPageTurnActive);
    if (params.font == nullptr) return;  // no reader font chain: cannot lay out
    if (freeink::book::ensureChapterSession(*ttf_, static_cast<uint16_t>(nextSpine), params, ttfGeneration) ==
        freeink::book::BookStatus::Ok) {
      freeink::book::pumpChapterChunk(*ttf_, static_cast<uint16_t>(nextSpine), BACKGROUND_BUILD_PAGES_PER_TICK);
      ttfPrefetchActive = true;
    }
  }
}

bool EpubReaderActivity::ttfPageTurn(const bool isForwardTurn) {
  if (!ttf_ || !epub) return false;
  // Navigation invalidates the displayed frame until the next successful render.
  ttfFrameRenderComplete.store(false, std::memory_order_release);

#ifdef READING_STATS_ENABLED
  uint32_t dwellSeconds = 0;
  const bool haveDwell = currentPageReadingSecondsForStats(dwellSeconds);
  if (SETTINGS.shouldTrackReadingStats()) {
    recordCurrentPageReadingTime();
    if (isForwardTurn && haveDwell) {
      recordForwardPagePaceSample(dwellSeconds, currentPageWordsOnPage);
      if (stats.totalPagesTurned < UINT32_MAX) stats.totalPagesTurned++;
      if (globalStats.totalPagesTurned < UINT32_MAX) globalStats.totalPagesTurned++;
      if (sessionPageTurns < UINT16_MAX) sessionPageTurns++;
    }
  }
#endif

  // P4 fast path: the framebuffer already holds this next page's content.
  // Advance state here on the loop task, then hand the display commit to
  // renderBookTtf via ttfUsePreRenderedBuffer — all display work (status
  // bar, gray planes, flush) stays on the render task under its RenderLock.
  // Validation is spine+page exact; anything else falls to the slow path.
  if (isForwardTurn && ttfPreRendered.ready && ttfPreRendered.spineIndex == currentSpineIndex &&
      ttfPreRendered.pageIndex == ttfPage + 1 && ttfPage + 1 < static_cast<int>(ttfPageCount)) {
    ttfPage = ttfPreRendered.pageIndex;
    ttfInvalidatePreRender();
    ttfUsePreRenderedBuffer = true;
    lastPageTurnTime = millis();
#ifdef READING_STATS_ENABLED
    pageShownAtMs = millis();
#endif
    logMemAt("page_turn");
    return true;
  }

  if (isForwardTurn) {
    const bool partialCurrent = ttf_->cacheReady() && ttf_->cachePartial() && ttfSpine == currentSpineIndex;
    if (ttfPage + 1 < static_cast<int>(ttfPageCount) ||
        (ttf_->sessionFor(static_cast<uint16_t>(currentSpineIndex)) && ttf_->sessionActive())) {
      ttfPage++;
      lastPageTurnTime = millis();
    } else if (partialCurrent) {
      // A partial FIBP is extendable, not the end of the chapter. Leave the
      // spine in place and let the next render pass resume the build.
      ttfPage = static_cast<int>(ttfPageCount);
      lastPageTurnTime = millis();
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
      ttfPage = 0;
      ttfRestoreLastPage = false;
      lastPageTurnTime = millis();
    } else {
      currentSpineIndex = epub->getSpineItemsCount();
      lastPageTurnTime = millis();
    }
  } else {
    if (ttfPage > 0) {
      ttfPage--;
      lastPageTurnTime = millis();
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      ttfPage = 0;
      ttfRestoreLastPage = true;
      lastPageTurnTime = millis();
    } else {
      return false;
    }
  }

#ifdef READING_STATS_ENABLED
  pageShownAtMs = millis();
#endif
  logMemAt("page_turn");
  return true;
}

#endif  // CROSSPOINT_TTF_READER

void EpubReaderActivity::onEndOfBookRendered() {
  automaticPageTurnActive = false;
  if (pendingSyncSaveError) {
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  clearDeferredReposition();
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();
  ImageBlock::clearRenderFailures();

  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  auto* fcm = renderer.getFontCacheManager();
  // BOOK_PROFILE: reset SD font overflow stats for this page turn.
  // The summary is logged at the end of the async display overlap block
  // below (phase=font_overflow).
#ifdef BOOK_PROFILE
  for (auto& [sdFontId, sdFont] : renderer.getSdCardFonts()) {
    sdFont->resetStats();
  }
#endif
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  // Scan the status bar too: a CJK book/chapter title redirected to the SD
  // fallback font joins the page's single batch prewarm instead of triggering
  // its own SD pass after the scope ends.
  renderStatusBar();
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  const bool needsTextGrayscale = !freeink::book::fontLoader.effectiveMonochrome();
#else
  const bool needsTextGrayscale = SETTINGS.textRenderMode == CrossPointSettings::TEXT_RENDER_SMOOTH;
#endif
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool absoluteImageGrayscale = pageHasImages && !gpio.deviceIsX3() &&
                                      display.getController() == HalDisplay::Controller::UC8279 &&
                                      renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Absolute).supported();
  const auto grayscale = renderer.grayscaleCapabilities(absoluteImageGrayscale ? HalDisplay::GrayscaleMode::Absolute
                                                                               : HalDisplay::GrayscaleMode::Overlay);
  const bool tiledGrayscale = needsAnyGrayscale && grayscale.stripUploads;
  // Paper Mono only (no other panel combines): defer the B/W base activation so
  // the gray planes join it in a single waveform. Displaying the base
  // separately makes the gray pass re-drive the whole text body — a visible
  // flash on every AA page.
  const bool combinedGrayscaleBase = tiledGrayscale && !pageHasImages && renderer.combinesGrayscaleBase();
  // supportsAsyncGrayscaleBase(): X3 must not use an async B/W refresh as the
  // grayscale base (upstream #3439); SSD1677-class panels keep the overlap.
  // The overlap window is also what lets the DUAL render walk fill both plane
  // buffers while the BW refresh is in flight (fork async-overlap work).
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncGrayscaleBase() && !pageHasImages;
  // GRAYSCALE_DUAL: one render walk flags both plane buffers. Gated to text
  // pages — image pages keep the two-pass walk until preserveImagePolarity
  // learns a dual target (design doc §8). Each path (tiled/nontiled) checks
  // its own buffer prerequisites before engaging. Absolute image pages
  // (upstream #3478) also always take the two-pass walk: dualPlane is false
  // when the page carries images, so the two feature paths never overlap.
  const bool dualPlane = !pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (absoluteImageGrayscale || needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    if (absoluteImageGrayscale) renderStatusBar();
  };

  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (absoluteImageGrayscale) {
    const auto baseMode = cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
    if (!renderer.displayGrayscaleBase(HalDisplay::GrayscaleMode::Absolute, baseMode)) {
      LOG_ERR("ERS", "Could not start absolute image page; displaying B/W");
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
      return;
    }
    LOG_DBG("ERS", "UC8279 image page: absolute quality waveform");
    pagesUntilFullRefresh = 1;
  } else if (pageHasImages) {
    // Image pages use one base refresh before the grayscale pass. FAST leaves
    // the panel receptive to the gray waveform; pending cleanup still honors
    // the scheduled/manual HALF refresh.
    renderer.displayBuffer(cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh = 1;
  } else if (combinedGrayscaleBase) {
    // Stash the base without activating; displayGrayBuffer() below commits
    // base + grays as one waveform.
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
  } else if (needsAnyGrayscale) {
    if (pagesUntilFullRefresh <= 1) {
      // A cleanup refresh settles X3 correctly only when its grayscale
      // preconditioning waveform runs before the gray planes are written.
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      renderer.preconditionGrayscale();
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else if (overlapRefresh) {
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, /*async=*/true);
    } else {
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh--;
    }
  } else {
    // Non-grayscale path: use async display when supported so we can overlap
    // the e-ink refresh (569-648ms) with background section builds and
    // next-chapter prefetch on the same core.
    const bool canAsyncDisplay = renderer.supportsAsyncRefresh() && !pageHasImages;
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, canAsyncDisplay);
    if (canAsyncDisplay) {
      // E-ink refresh is running. While it completes, run background work.
#ifdef BOOK_PROFILE
      const uint32_t overlapStartMs = millis();
      const uint8_t overlapCore = xPortGetCoreID();
#endif
      prefetchNextChapterDuringDisplay();
      if (section && section->isBuilding() && !section->isBuildComplete() && buildTickHeapGate()) {
        section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK);
      }
      renderer.waitRefreshComplete();
#ifdef BOOK_PROFILE
      LOG_DBG("PROF", "phase=async_display_overlap dur=%lums core=%d prefetch_active=%d build_active=%d",
              millis() - overlapStartMs, overlapCore, nextSectionPrefetch ? 1 : 0,
              section && section->isBuilding() ? 1 : 0);
#endif
    }
  }
  // BOOK_PROFILE: define a summary lambda for SD font overflow misses.
  // Each miss = SD read (~5-20ms); the goal of the PSRAM font cache feature
  // is to drive this to 0 for CJK books by raising MAX_PAGE_GLYPHS and expanding the ring.
  // Called at the end of renderContents() (after all gray paths) and before
  // the early return on storeBwBuffer() failure.
#ifdef BOOK_PROFILE
  auto logOverflowSummary = [&] {
    const auto& sdFonts = renderer.getSdCardFonts();
    uint32_t totalOverflowMisses = 0;
    uint32_t maxOverflowFill = 0;
    for (const auto& [sdFontId, sdFont] : sdFonts) {
      const auto& stats = sdFont->getStats();
      totalOverflowMisses += stats.overflowMisses;
      if (stats.overflowCountAtLog > maxOverflowFill) maxOverflowFill = stats.overflowCountAtLog;
    }
    LOG_DBG("PROF", "phase=font_overflow misses=%u max_fill=%u/%u", totalOverflowMisses, maxOverflowFill,
            SdCardFont::getOverflowCapacity());
  };
#endif
  const auto tDisplay = millis();
  // Path selection trace: which gray route this page takes and why.
  LOG_DBG("ERS", "Gray path: tiled=%d dual=%d textAA=%d images=%d overlap=%d", tiledGrayscale, dualPlane,
          needsTextGrayscale, pageHasImages, overlapRefresh);
  // Full-frame gray plane size for both dual paths (tiled async buffers and
  // nontiled dual): panel-stride bytes × full display height.
  const int dualHeight = renderer.getDisplayHeight();
  const size_t planeBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * dualHeight;
  // Heap gate shared by every dual allocation (async overlap buffers, the
  // 8KB dual scratch, and the nontiled 2x48KB planes).
  constexpr size_t PLANE_BUF_HEADROOM = 60000;
  constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
  const auto planeBufFits = [planeBytes] {
    return ESP.getFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
           ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
  };

  if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = dualHeight;
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
    };

    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      if (msbPlaneBuf && dualPlane) {
        // Async overlap, both buffers live: one DUAL walk fills both planes
        // while the BW refresh is in flight.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_DUAL);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(lsbPlaneBuf.get() + static_cast<size_t>(y) * gwBytes, y, rows,
                                    msbPlaneBuf.get() + static_cast<size_t>(y) * gwBytes);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
        }
      } else {
        renderPlaneToBuffer(true, lsbPlaneBuf.get());
        if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      }
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
    } else {
      // Dual needs both plane bands live for the whole walk; a shared 8 KB
      // scratch cannot hold two bands. Allocate the required scratch FIRST,
      // then attempt the optional MSB scratch — if memory only fits one band,
      // the two-pass fallback still runs instead of skipping AA.
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      auto msbScratch = (dualPlane && scratch && planeBufFits())
                            ? makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS)
                            : nullptr;
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh || combinedGrayscaleBase) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents. On the combined-base path the
          // base activation is still deferred; this cleanup commits it so the
          // page reaches the panel even without its grays.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else if (msbScratch) {
        // One DUAL walk per band: LSB bits land in `scratch`, MSB in `msbScratch`.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_DUAL);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows, msbScratch.get());
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
          renderer.writeGrayscalePlaneStrip(false, msbScratch.get(), y, rows);
        }
        const auto tGrayBoth = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled dual): prewarm=%lums bw_render=%lums display=%lums gray_both=%lums "
                "gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayBoth - tDisplay,
                tGrayDisplay - tGrayBoth, tCleanup - tGrayDisplay, tEnd - t0);
      } else {
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      }
    }
  } else {
    if (needsAnyGrayscale) {
      // Nontiled dual: the gray planes are rendered into two private
      // full-frame buffers and copied to the driver explicitly, so the BW
      // base can stay in the framebuffer for displayGrayBuffer(). Costs
      // 2x 48KB transient heap — gated by planeBufFits() like the async
      // overlap buffers; OOM falls back to the classic framebuffer passes.
      const bool nontiledDual = dualPlane && planeBufFits();
      auto lsbPlane = nontiledDual ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
      auto msbPlane = (lsbPlane && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
      if (!msbPlane) {
        lsbPlane = nullptr;
      }
      if (!nontiledDual || !msbPlane) {
        LOG_DBG("ERS", "Nontiled dual unavailable (heap); using framebuffer gray passes");
      }

      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        if (absoluteImageGrayscale) renderer.setRenderMode(GfxRenderer::BW);
#ifdef BOOK_PROFILE
        logOverflowSummary();
#endif
        return;
      }
      const auto tBwStore = millis();

      if (lsbPlane && msbPlane) {
        // One DUAL walk over a "full-frame strip" (origin 0, panelHeight rows):
        // drawGrayDualPixel's rotate+clip then lands each tone's plane bits in
        // the two private buffers. The BW base stays in the framebuffer so
        // displayGrayBuffer() can stream it as usual.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_DUAL);
        renderer.beginStripTarget(lsbPlane.get(), 0, dualHeight, msbPlane.get());
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        const auto tGrayBoth = millis();

        renderer.copyGrayscaleLsbBuffers(lsbPlane.get());
        renderer.copyGrayscaleMsbBuffers(msbPlane.get());
        const auto tGrayCopy = millis();

        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.restoreBwBuffer();
        memSentinelCheck("ttf gray nontiled-dual");
        const auto tBwRestore = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (nontiled dual): prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
                "gray_both=%lums gray_copy=%lums gray_display=%lums bw_restore=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayBoth - tBwStore,
                tGrayCopy - tGrayBoth, tGrayDisplay - tGrayCopy, tBwRestore - tGrayDisplay, tEnd - t0);
      } else {
        // Two-pass fallback (also the absolute-image path — DUAL requires
        // dualPlane, which is false on image pages): absolute planes seed with
        // 0xFF so the base image's black/white bits survive in both planes.
        renderer.clearScreen(absoluteImageGrayscale ? 0xFF : 0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        renderGrayscalePass();
        renderer.copyGrayscaleLsbBuffers();
        const auto tGrayLsb = millis();

        renderer.clearScreen(absoluteImageGrayscale ? 0xFF : 0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        renderGrayscalePass();
        renderer.copyGrayscaleMsbBuffers();
        const auto tGrayMsb = millis();

        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.restoreBwBuffer();
        const auto tBwRestore = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
                "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
                tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
      }
    } else {
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
#ifdef BOOK_PROFILE
  logOverflowSummary();
#endif
}

void EpubReaderActivity::renderStatusBar() const {
  // Legacy path reads the Section; the TTF path keeps nextPageNumber /
  // cachedChapterTotalPageCount in sync as its mirrors.
  const int currentPage = section ? section->currentPage + 1 : nextPageNumber + 1;
  const float pageCount =
      section ? section->estimatedTotalPages() : (cachedChapterTotalPageCount > 0 ? cachedChapterTotalPageCount : 1);
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub ? (epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100) : 0;

  std::string title;
  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    if (epub) {
      const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
      if (tocIndex != -1) {
        const auto tocItem = epub->getTocItem(tocIndex);
        title = tocItem.title;
      }
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub ? epub->getTitle() : "";
  }

#ifdef READING_STATS_ENABLED
  char chapterTimeLeftBuf[24];
  const char* chapterTimeLeft = nullptr;
  if (sb.showChapterTimeLeft) {
    if (SETTINGS.shouldTrackReadingStats() &&
        (section
#if defined(CROSSPOINT_TTF_READER)
         || ttf_
#endif
         ) &&
        (section ? section->estimatedTotalPages() > 0 : cachedChapterTotalPageCount > 0)) {
      const int chapterTotalPages =
          section ? static_cast<int>(section->estimatedTotalPages()) : cachedChapterTotalPageCount;
      const int chapterCurrentPage = section ? section->currentPage : nextPageNumber;
      const int pagesRemaining = std::max(0, chapterTotalPages - chapterCurrentPage - 1);
      auto timeLeft = estimateChapterTimeLeftSeconds(stats, globalStats, static_cast<uint16_t>(pagesRemaining),
                                                     currentPageWordsOnPage);
      if (timeLeft) {
        formatChapterTimeLeft(*timeLeft, chapterTimeLeftBuf, sizeof(chapterTimeLeftBuf));
        chapterTimeLeft = chapterTimeLeftBuf;
      } else {
        // Pace not learned yet (fewer than 50 global / 10 book page turns recorded).
        // Show a localized placeholder so the slot reads as active rather than broken.
        chapterTimeLeft = tr(STR_TIME_LEFT_UNAVAILABLE);
      }
    } else {
      // Tracking off but the element is enabled: keep the slot visible per the
      // user's chosen position so the setting is clearly doing something.
      chapterTimeLeft = tr(STR_TIME_LEFT_UNAVAILABLE);
    }
  }
#else
  const char* chapterTimeLeft = nullptr;
#endif

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section ? section->isBuilding() : false, chapterTimeLeft);
}

// ---------------------------------------------------------------------------
// Toolbar reader menu
// ---------------------------------------------------------------------------

namespace {
constexpr StrId kTextRowNames[] = {StrId::STR_FONT, StrId::STR_FONT_SIZE, StrId::STR_LINE_SPACING,
                                   StrId::STR_PARA_ALIGNMENT, StrId::STR_FOCUS_READING};
constexpr StrId kSpacingIds[] = {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE, StrId::STR_EXTRA_WIDE};
constexpr StrId kAlignIds[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                               StrId::STR_BOOK_S_STYLE};
constexpr int kTextRowCount = static_cast<int>(std::size(kTextRowNames));
static_assert(std::size(kSpacingIds) == CrossPointSettings::LINE_COMPRESSION_COUNT, "line spacing labels");
static_assert(std::size(kAlignIds) == CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, "alignment labels");
}  // namespace

bool EpubReaderActivity::usesToolbarMenu() const {
  // Both board classes drive the same chrome: touch through the FreeInkUI tap
  // targets, buttons through the focused-tool pill and the panel cursor.
  return SETTINGS.readerMenuStyle == CrossPointSettings::READER_MENU_TOOLBAR;
}

std::string EpubReaderActivity::currentChapterTitle() const {
  if (!epub) return "";
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex != -1) {
    return epub->getTocItem(tocIndex).title;
  }
  return tr(STR_UNNAMED);
}

std::string EpubReaderActivity::textRowName(int row) const {
  return row >= 0 && row < kTextRowCount ? I18N.get(kTextRowNames[row]) : "";
}

std::string EpubReaderActivity::textRowValue(int row) const {
  static constexpr StrId kFamily[] = {StrId::STR_NOTO_SERIF, StrId::STR_ATKINSON_HN, StrId::STR_ATKINSON_HN};
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    switch (row) {
      case 0:
        return SETTINGS.ttfFontFamilyName[0] != '\0' ? SETTINGS.ttfFontFamilyName : tr(STR_BUILTIN_FONT);
      case 1:
        return std::to_string(SETTINGS.ttfFontPointSize) + " pt";
      case 2:
        return I18N.get(kSpacingIds[SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT]);
      case 3:
        return I18N.get(kAlignIds[SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT]);
      case 4:
        return SETTINGS.focusReadingEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      default:
        return "";
    }
  }
#endif
  switch (row) {
    case 0:
      if (SETTINGS.sdFontFamilyName[0] != '\0') return SETTINGS.sdFontFamilyName;
      return I18N.get(kFamily[SETTINGS.fontFamily % CrossPointSettings::FONT_FAMILY_COUNT]);
    case 1:
      return std::to_string(SETTINGS.fontPointSize) + " pt";
    case 2:
      return I18N.get(kSpacingIds[SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT]);
    case 3:
      return I18N.get(kAlignIds[SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT]);
    case 4:
      return SETTINGS.focusReadingEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    default:
      return "";
  }
}

// Live apply: persist, re-paginate, and let renderBook() redraw the page with
// the open panel back on top -- the book itself is the preview.
void EpubReaderActivity::applyTextSettingLive() {
  applyReaderTextSettings();
  discardOverlayPage();  // the stored page is laid out with the old settings
  requestUpdate();
}

// Settings-style option pickers for the Text panel's enum rows. Every
// selection applies immediately to the page under the sheet.
#if defined(CROSSPOINT_TTF_READER)

// The quick sheet relays out only the page around the displayed char anchor.
// It is a transient ChapterLayout pass (TextSettingsPreview's scratch pattern),
// never a committed cache rebuild; the full reflow happens once on close.
void EpubReaderActivity::openFontSheet() {
  if (!ttf_) return;
  openOverlay(Overlay::FontSheet);
}

void EpubReaderActivity::openFontFamilyPicker() {
  if (!ttf_) return;

  // Same enum-picker pattern as the Text panel: one modal over the quick
  // sheet. Built-in is index 0; scanned families follow in loader order.
  const uint8_t familyCount = freeink::book::fontLoader.familyCount();
  std::vector<std::string> options;
  options.reserve(1U + familyCount);
  options.emplace_back(tr(STR_BUILTIN_FONT));
  for (uint8_t i = 0; i < familyCount; ++i) {
    options.emplace_back(freeink::book::fontLoader.families()[i].name);
  }

  int currentIndex = 0;
  if (SETTINGS.ttfFontFamilyName[0] != '\0') {
    const auto* current = freeink::book::fontLoader.findFamily(SETTINGS.ttfFontFamilyName);
    if (current != nullptr) currentIndex = 1 + (current - freeink::book::fontLoader.families());
  }

  overlayPopup.show(StrId::STR_FONT_FAMILY, options, currentIndex, [this](const int idx) {
    if (idx <= 0) {
      SETTINGS.ttfFontFamilyName[0] = '\0';
    } else if (idx <= freeink::book::fontLoader.familyCount()) {
      const auto& family = freeink::book::fontLoader.families()[idx - 1];
      if (!freeink::book::fontLoader.isFamilyAvailable(family)) return;
      strncpy(SETTINGS.ttfFontFamilyName, family.name, sizeof(SETTINGS.ttfFontFamilyName) - 1);
      SETTINGS.ttfFontFamilyName[sizeof(SETTINGS.ttfFontFamilyName) - 1] = '\0';
    } else {
      return;
    }
    SETTINGS.readerFontEngine = CrossPointSettings::READER_ENGINE_TTF;
    freeink::book::fontLoader.selectFamily(SETTINGS.ttfFontFamilyName);
    // The popup-dismiss handler performs the page-only relayout once, with the
    // sheet still open. Avoid a second FAST refresh from inside the callback.
    quickFontFamilyPending = true;
  });
  paintOverlayPopup();
}

void EpubReaderActivity::quickFontSelectRow(const int row, const bool refresh) {
  quickFontRow = std::clamp(row, 0, 1);
  if (!refresh || !toolbarUi) return;
  RenderLock lock;
  settleOverlayRefresh();
  renderOverlay();
  pushOverlayRefresh();
}

void EpubReaderActivity::quickFontStep(const int direction) {
  if (!ttf_ || quickFontRow != 0) return;
  const int next = std::clamp(static_cast<int>(SETTINGS.ttfFontPointSize) + direction,
                              static_cast<int>(CrossPointSettings::TTF_FONT_POINT_SIZE_MIN),
                              static_cast<int>(CrossPointSettings::TTF_FONT_POINT_SIZE_MAX));
  if (next == SETTINGS.ttfFontPointSize) return;
  SETTINGS.ttfFontPointSize = static_cast<uint8_t>(next);
  renderQuickFontPage();
}

void EpubReaderActivity::renderQuickFontPage() {
  if (!ttf_ || !epub) return;

  freeink::book::LayoutParams params;
  ttf_->makeLayoutParams(renderer, params, automaticPageTurnActive);
  if (params.font == nullptr) {
    LOG_ERR("ERS", "Quick font reflow: no font chain");
    return;
  }

  // One backing buffer per sheet session: allocated on the first relayout,
  // freed on close, so taps never churn the PSRAM pool. Without it the sink
  // falls back to painting every scanned page inline.
  if (!quickFontPreview.attached()) {
    if (!quickFontPreviewBuf) quickFontPreviewBuf = poolMakeBytes(QuickPageCapture::kBufferBytes);
    if (quickFontPreviewBuf) {
      quickFontPreview.attach(quickFontPreviewBuf.get(), QuickPageCapture::kBufferBytes);
    } else {
      LOG_ERR("ERS", "OOM: quick font preview buffer");
    }
  }

  const uint32_t targetChar = ttfCurrentCharStart;
  bool found = false;
  uint32_t pageIndex = 0;
  class QuickSink final : public freeink::book::PageSink {
   public:
    QuickSink(EpubReaderActivity* owner, const void* font, const uint32_t target, const uint8_t maxPages,
              bool& foundRef, uint32_t& pageIndexRef, QuickPageCapture& captureRef)
        : owner_(owner),
          font_(font),
          target_(target),
          maxPages_(maxPages),
          found_(foundRef),
          pageIndex_(pageIndexRef),
          capture_(captureRef) {}

    bool onPage(const freeink::book::Page& page) override {
      if (page.charStart > target_) {
        // The previous captured page is the target page: it ended before the
        // first page past the anchor. Stop with a confirmed preview.
        if (sawCandidate_) found_ = true;
        return false;
      }
      // Later pages also match until the first page past the anchor, so only
      // the last capture matters. Pages are captured (deep-copied out of the
      // engine's per-page arena) instead of painted here; the target page is
      // painted once after the scan. A page that does not fit the capture
      // buffer is painted inline, as the pre-capture path did.
      sawCandidate_ = true;
      pageIndex_ = page.pageIndex;
      if (!capture_.capture(page)) {
        owner_->renderer.clearScreen(0xFF);
        owner_->paintTtfPage(page, const_cast<void*>(font_));
        capture_.reset();
      }
      if (page.pageIndex + 1 >= maxPages_) {
        budgetStopped_ = true;
        return false;
      }
      return true;
    }

    bool sawCandidate() const { return sawCandidate_; }
    bool budgetStopped() const { return budgetStopped_; }

   private:
    EpubReaderActivity* owner_;
    const void* font_;
    uint32_t target_;
    uint8_t maxPages_;
    bool& found_;
    uint32_t& pageIndex_;
    QuickPageCapture& capture_;
    bool sawCandidate_ = false;
    bool budgetStopped_ = false;
  };
  // The scan is paint-free (pages are captured, not painted), so the budget
  // bounds only layout work; a deeper budget keeps more chapters on the fast
  // page-only path instead of the full-reflow fallback.
  constexpr uint8_t kQuickRelayoutPageBudget = 128;
  QuickSink sink(this, params.font, targetChar, kQuickRelayoutPageBudget, found, pageIndex, quickFontPreview);

  {
    RenderLock lock;
    settleOverlayRefresh();  // a deferred chrome refresh may still be running
    const auto st =
        ttf_->quickLayoutPage(static_cast<uint16_t>(currentSpineIndex), params, sink, kQuickRelayoutPageBudget);
    // A page past the anchor confirms the last captured page as the target;
    // a natural end-of-chapter without that confirmation means the anchor
    // page itself was the last page. Budget exhaustion always falls back.
    if (sink.sawCandidate() && !sink.budgetStopped()) found = true;
    if (!found) {
      LOG_DBG("ERS", "Quick font reflow did not reach anchor (%s); falling back to full reflow", bookStatusName(st));
    } else if (quickFontPreview.ready()) {
      // Paint the captured target page once. Base-only preview by design
      // (§6): the final close reflow restores the AA gray planes so per-tap
      // cost stays FAST.
      renderer.clearScreen(0xFF);
      paintTtfPage(quickFontPreview.page(), params.font);
    }
    // With the capture not ready the target page was painted inline by the
    // sink; the framebuffer already shows it.
  }

  if (!found) {
    applyReaderTextSettings();
    requestUpdate();
    return;
  }

  // The page-only layout succeeded. Mirror the page cursor for the status bar,
  // snapshot the new clean page for overlay transitions, then draw the sheet.
  // The sheet preview is intentionally base-only (design §6): the final close
  // reflow restores the AA gray planes so per-tap cost stays FAST.
  LOG_DBG("GRS", "quickFont preview: base-only FAST (AA restored on close)");
  ttfPage = static_cast<int>(pageIndex);
  nextPageNumber = ttfPage;
  {
    RenderLock lock;
    renderStatusBar();
    if (renderer.hasFrameBuffer()) {
      if (overlayPageStored) {
        renderer.discardStoredBwBuffer();
        overlayPageStored = false;
      }
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
    pushOverlayRefresh();
  }
}

void EpubReaderActivity::closeFontSheet() {
#if defined(CROSSPOINT_TTF_READER)
  // Teardown is unconditional: a FontSheet left in `overlay` keeps
  // isChromeOpen() true, so every back gesture re-enters this close path and
  // is swallowed with no visible effect. Only the reflow work needs ttf_.
  overlay = Overlay::None;
  overlayPopup.dismiss();
  quickFontFamilyPending = false;
  quickFontPreview.attach(nullptr, 0);  // buffer freed below
  quickFontPreviewBuf.reset();
  discardOverlayPage();
  requestUpdate();
  if (!ttf_) return;
  applyReaderTextSettings();  // one persisted save + full reflow on close
  // The sheet hid a full-page relayout; ask the next render for a cleanup
  // cycle rather than leaving a differential overlay refresh in the cadence.
  pagesUntilFullRefresh = 1;
#endif
}
#endif

void EpubReaderActivity::showTextRowPopup(const int row) {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_ && (row == 0 || row == 1)) {
    // Size and family live in the compact quick sheet; the full-screen
    // settings activity remains the advanced Settings entry point.
    openFontSheet();
    return;
  }
#endif
  switch (row) {
    case 0: {
      // Non-TTF builds (and a TTF build whose runtime failed to open) keep
      // the full-screen family picker; only a live TTF runtime uses the sheet.
      auto settings = makeUniqueNoThrow<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                              TextSettingsActivity::Tab::Family);
      if (!settings) {
        LOG_ERR("ERS", "OOM: text settings activity");
        return;
      }
      overlay = Overlay::None;
      overlayPopup.dismiss();
      discardOverlayPage();
      startActivityForResult(std::move(settings), [this](const ActivityResult&) {
        applyReaderTextSettings();
        overlay = Overlay::Text;
        panelIndex = 0;
        if (toolbarUi) toolbarUi->begin();
        requestUpdate();
      });
      return;
    }
    case 1: {
      // The point sizes the active family actually ships.
      const auto sizes = readerFontPointSizes(&sdFontSystem.registry(), SETTINGS.sdFontFamilyName);
      if (sizes.empty()) return;
      std::vector<std::string> labels;
      labels.reserve(sizes.size());
      for (const uint8_t size : sizes) labels.push_back(std::to_string(size) + " pt");
      const uint8_t cur = snapToNearestPointSize(sizes, SETTINGS.fontPointSize);
      int curIdx = 0;
      for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] == cur) curIdx = static_cast<int>(i);
      }
      overlayPopup.show(StrId::STR_FONT_SIZE, labels, curIdx, [this, sizes](int idx) {
        if (idx < 0 || idx >= static_cast<int>(sizes.size())) return;
        SETTINGS.fontPointSize = sizes[idx];
        applyTextSettingLive();
      });
      break;
    }
    case 2:
      overlayPopup.show(StrId::STR_LINE_SPACING, kSpacingIds, static_cast<int>(std::size(kSpacingIds)),
                        SETTINGS.lineSpacing % CrossPointSettings::LINE_COMPRESSION_COUNT, [this](int idx) {
                          SETTINGS.lineSpacing = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    case 3:
      overlayPopup.show(StrId::STR_PARA_ALIGNMENT, kAlignIds, static_cast<int>(std::size(kAlignIds)),
                        SETTINGS.paragraphAlignment % CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, [this](int idx) {
                          SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx);
                          applyTextSettingLive();
                        });
      break;
    default:
      return;
  }
  paintOverlayPopup();
}

void EpubReaderActivity::discardOverlayPage() {
  if (!overlayPageStored) return;
  renderer.discardStoredBwBuffer();
  overlayPageStored = false;
}

// Upper bound on how long settleOverlayRefresh() polls the panel before giving
// up on a deferred overlay refresh: a healthy FAST waveform completes in
// ~0.3-2 s, so exceeding this means the controller is wedged, not slow.
static constexpr uint32_t OVERLAY_REFRESH_SETTLE_TIMEOUT_MS = 3000;

// Push freshly painted overlay chrome. Where the panel supports it the refresh
// is fired deferred: the loop keeps polling input while the waveform runs, so
// the chrome answers taps and buttons the moment it is visible instead of only
// after a blocking displayBuffer() returns. Caller must hold the RenderLock.
void EpubReaderActivity::pushOverlayRefresh() {
  if (renderer.supportsAsyncRefresh()) {
    if (renderer.refreshBusy()) {
      // A previous deferred refresh is still running (its settle timed out on
      // a wedged panel): firing another waveform into it would re-enter the
      // driver's no-timeout busy wait. Skip; the pending flag stays set so a
      // later settle/push repaints once the panel is idle.
      LOG_ERR("ERS", "overlay refresh still busy, skipping push");
      return;
    }
    renderer.displayBufferAsync(HalDisplay::FAST_REFRESH);
    overlayRefreshPending.store(true, std::memory_order_release);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

// Wait out a pending deferred overlay refresh and reseed the panel's
// differential baseline from the framebuffer: the shadow-free async path skips
// the post-refresh resync, so without this the next FAST diff would run
// against the frame from before the chrome and leave stale pixels on the
// glass. The panel's own busy wait (UC8279 UcIdleHigh in the driver) has
// deliberately no timeout, so poll refreshBusy() here under a hard deadline
// instead: on timeout the controller is wedged, and we arm a driver resync
// (flag only, no bus traffic) for the next refresh rather than spin forever
// under the RenderLock (issue #143 freeze). After a timeout, subsequent
// settles only single-check refreshBusy() — polling another 3 s for every
// overlay interaction would stall input while the wedge lasts (#151). Caller
// must hold the RenderLock.
void EpubReaderActivity::settleOverlayRefresh() {
  if (!overlayRefreshPending.load(std::memory_order_acquire)) return;
  if (overlaySettleTimedOut) {
    // A previous settle already burned the full deadline on this refresh: one
    // non-blocking check only, never re-poll under the RenderLock.
    if (renderer.refreshBusy()) return;
  } else {
    const uint32_t start = millis();
    while (renderer.refreshBusy()) {
      if (millis() - start > OVERLAY_REFRESH_SETTLE_TIMEOUT_MS) {
        LOG_ERR("ERS", "overlay refresh busy >%lu ms, arming panel resync",
                static_cast<unsigned long>(OVERLAY_REFRESH_SETTLE_TIMEOUT_MS));
        renderer.requestResync();
        overlaySettleTimedOut = true;
        // Leave the flag set: the next settle re-attempts (single check), and
        // pushOverlayRefresh() refuses to fire a new waveform while this one
        // is still running.
        return;
      }
      vTaskDelay(1);
    }
  }
  overlaySettleTimedOut = false;
  renderer.cleanupGrayscaleWithFrameBuffer();  // waits, then reseeds the baseline
  // Clear only after the baseline reseed: while it runs, the refresh is not
  // yet fully drained as far as the next settle/push is concerned.
  overlayRefreshPending.store(false, std::memory_order_relaxed);
}

// Recover a refresh that was skipped while the panel was wedged (timed-out
// settle): without this, a deferred overlay close would leave the chrome on
// the glass until the next page turn, because nothing else schedules a render
// once the panel drains (#151 review). Throttled non-blocking re-checks; the
// actual settle/push runs under the RenderLock like every other path.
void EpubReaderActivity::retryDeferredOverlayPush() {
  if (!overlayRefreshPending.load(std::memory_order_acquire) || !overlaySettleTimedOut) return;
  constexpr uint32_t OVERLAY_PUSH_RETRY_MS = 500;
  const uint32_t now = millis();
  if (now - overlayPushRetryMs < OVERLAY_PUSH_RETRY_MS) return;
  overlayPushRetryMs = now;
  if (renderer.refreshBusy()) return;  // still wedged; retry on a later tick
  RenderLock lock;
  settleOverlayRefresh();  // drains the flag and reseeds the baseline
  if (!renderer.refreshBusy()) renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void EpubReaderActivity::openOverlay(Overlay target) {
  mappedInput.resetHomeButtonInput();
  const Overlay previous = overlay;
  overlay = target;
  if (!toolbarUi) toolbarUi = std::make_unique<ReaderToolbarUi>(renderer);
  if (previous == Overlay::None) toolbarUi->begin();
#if defined(CROSSPOINT_TTF_READER)
  LOG_DBG("ERS", "overlay open target=%d previous=%d uiReady=%d", static_cast<int>(target), static_cast<int>(previous),
          toolbarUi->routingReady());
#endif
  // Buttons show a cursor from the start; touch boards only once a button moves it.
  panelCursorShown = !mappedInput.hasTouch();
  switch (target) {
    case Overlay::Toolbar:
      focusedTool = 0;
      break;
    case Overlay::Contents:
      panelIndex = std::max(0, epub->getTocIndexForSpineIndex(currentSpineIndex));
      // Fresh viewport opening on the current chapter, cursor shown or not.
      toolbarUi->nav().reset(panelIndex);
      toolbarUi->nav().top = panelIndex;
      break;
    case Overlay::Text:
      panelIndex = 0;
      toolbarUi->nav().reset();
      break;
    case Overlay::More:
      panelIndex = 0;
      buildMoreActions();
      toolbarUi->nav().reset();
      break;
#if defined(CROSSPOINT_TTF_READER)
    case Overlay::FontSheet:
      quickFontRow = 0;
      break;
#endif
    default:
      break;
  }
  panelHoldJumped = false;

  // The page is already on screen and still in the framebuffer, so paint the
  // chrome straight onto it and push one refresh. requestUpdate() would
  // re-render the whole page first: slow, and visibly wrong, since that repaint
  // lands before the overlay does.
  //
  // Refresh mode: FAST for every overlay paint, first open included. The AA
  // pass only grays glyph edges, and residue a FAST differential leaves under
  // the sheet has not shown in practice; it also self-heals on the
  // Xteink-class panels, whose close path re-renders the page. If text or
  // images ever visibly ghost through the chrome, restore a HALF cleanup on
  // the first open (see #2190 for the mechanism).
  bool hasRenderedPage = section != nullptr;
#if defined(CROSSPOINT_TTF_READER)
  // The TTF path keeps no Section mirror. Use only a frame that renderBookTtf
  // has completed for the current chapter; failed reads/navigation clear the
  // flag so a stale framebuffer can never be reused.
  hasRenderedPage = hasRenderedPage || (ttf_ && ttfFrameRenderComplete.load(std::memory_order_acquire) &&
                                        ttfSpine == currentSpineIndex && ttfPage >= 0 && ttfPageCount > 0);
#endif
  if (hasRenderedPage) {
    // Serialize against the render task: renderBook may be mid-page (status
    // bar included) in the shared framebuffer, and painting the chrome from
    // the loop task at the same time interleaves the two frames.
    RenderLock lock;
    settleOverlayRefresh();
    if (previous == Overlay::None) {
      // Snapshot the clean page so stepping back from a panel to the toolbar
      // (and closing, where supported) can restore it without a re-render.
      overlayPageStored = renderer.storeBwBuffer();
    } else if (overlayPageStored) {
      // Overlay -> overlay: wipe the previous chrome (toolbar header, sheet,
      // progress row) back to the clean page so none of it shows around or
      // through the new sheet; re-store for the next transition. No baseline
      // resync: the glass still shows the old chrome, and the differential
      // must keep diffing against it to erase it.
      renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
      overlayPageStored = renderer.storeBwBuffer();
    }
    renderOverlay();
#if defined(CROSSPOINT_TTF_READER)
    LOG_DBG("ERS", "overlay rendered=%d uiReady=%d", static_cast<int>(overlay), toolbarUi->routingReady());
    memSentinelCheck("overlay render");
    LOG_DBG("ERS", "overlay mem: HeapFree=%u HeapMin=%u MaxAlloc=%u PSRAMFree=%u loopStackHW=%u",
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(ESP.getFreePsram()),
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
    pushOverlayRefresh();
  } else {
    requestUpdate();  // no page yet: renderBook() draws the overlay once it is
  }
}

// Close the overlay back to the reading page. Boards without the Xteink
// grayscale-AA pass restore the page snapshot and push one FAST refresh -- no
// re-render, no flash; Xteink boards re-render to restore the AA planes.
void EpubReaderActivity::closeOverlayToPage() {
  mappedInput.resetHomeButtonInput();
#if defined(CROSSPOINT_TTF_READER)
  // FontSheet owns its close contract: persist once and force the full reflow.
  // The generic overlay close cannot handle the page-only preview state.
  if (overlay == Overlay::FontSheet) {
    closeFontSheet();
    return;
  }
#endif
  overlay = Overlay::None;
  overlayPopup.dismiss();  // an option picker cannot outlive its panel
  toolbarUi.reset();       // ~1 KB of interaction table + props, only needed while open
  if (!xteinkClassPanel() && overlayPageStored) {
    RenderLock lock;  // the render task shares the framebuffer
    settleOverlayRefresh();
    // No baseline resync: the glass shows the chrome, and erasing it needs
    // the differential to keep diffing against the last pushed frame.
    renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
    overlayPageStored = false;
    if (!renderer.refreshBusy()) {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      // Timed-out settle on a wedged panel: entering the driver's no-timeout
      // display wait here would re-create the #143 freeze on a back gesture.
      // Skip the push; the pending flag keeps this close deferred and
      // retryDeferredOverlayPush() (activity loop) pushes the clean page once
      // the panel drains and reseeds the baseline.
      LOG_ERR("ERS", "overlay close deferred, panel still busy");
    }
    return;
  }
  discardOverlayPage();
  requestUpdate();  // redraw the clean page
}

void EpubReaderActivity::renderOverlay() {
#if defined(CROSSPOINT_TTF_READER)
  if (!epub || !toolbarUi || (!section && !ttf_)) return;
#else
  if (!epub || !section || !toolbarUi) return;
#endif

  ReaderToolbarUi::Model model;
  // The toolbar's tool pill is the button-navigation cursor: tap-first (same
  // convention as the panel lists), it only shows once a button has moved it.
  // Panels override below: there the pill marks the open panel on every board.
  model.activeTool = (overlay == Overlay::Toolbar && !panelCursorShown) ? -1 : focusedTool;
  // Strings the model points at live here until render() returns.
  std::string chapterTitle, pageInfo;
#if defined(CROSSPOINT_TTF_READER)
  std::string sizeText, familyText;
#endif

  if (overlay == Overlay::Toolbar) {
    chapterTitle = currentChapterTitle();
    // Legacy reads the Section; the TTF path reads its position mirrors.
    const int pageCount = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
    const int chapterCurrentPage = section ? section->currentPage + 1 : nextPageNumber + 1;
    const float chapterProgress =
        pageCount > 0 ? static_cast<float>(chapterCurrentPage) / static_cast<float>(pageCount) : 0.0f;
    const float bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress);
    pageInfo = std::to_string(chapterCurrentPage) + "/" + std::to_string(pageCount) + "   " +
               std::to_string(clampPercent(static_cast<int>(bookProgress * 100.0f + 0.5f))) + "%";
    model.chapterTitle = chapterTitle.c_str();
    model.pageInfo = pageInfo.c_str();
    model.progressPermille = static_cast<int>(bookProgress * 1000.0f + 0.5f);
    toolbarUi->setModel(model);
    toolbarUi->render();
    return;
  }

#if defined(CROSSPOINT_TTF_READER)
  if (overlay == Overlay::FontSheet) {
    model.quickFont = true;
    model.panelTitle = tr(STR_FONT);
    model.quickSelected = quickFontRow;
    model.bottomReserve = mappedInput.hasTouch() ? 0 : UITheme::getInstance().getMetrics().buttonHintsHeight;
    sizeText = std::to_string(SETTINGS.ttfFontPointSize) + " pt";
    familyText = SETTINGS.ttfFontFamilyName[0] != '\0' ? SETTINGS.ttfFontFamilyName : tr(STR_BUILTIN_FONT);
    model.sizeText = sizeText.c_str();
    model.familyText = familyText.c_str();
    toolbarUi->setModel(model);
    toolbarUi->render();
    if (!mappedInput.hasTouch()) {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    return;
  }
#endif

  // Panels (Contents / Text / More): a bottom sheet over the page.
  model.panel = true;
  if (!mappedInput.hasTouch()) {
    model.denseRows = true;
  }
  // Tap-first: the cursor is only drawn once a button has moved it, so a
  // tapped row does not stay inverted after its action.
  model.selectedIndex = panelCursorShown ? panelIndex : -1;
  if (overlay == Overlay::Contents) {
    model.panelTitle = tr(STR_TOOL_CONTENTS);
    model.itemCount = epub->getTocItemsCount();
    model.rowText = [this](int i) {
      const auto item = epub->getTocItem(i);
      const int depth = item.level > 1 ? (item.level - 1) * 2 : 0;
      return std::string(depth, ' ') + item.title;
    };
  } else if (overlay == Overlay::Text) {
    model.panelTitle = tr(STR_TOOL_TEXT);
    model.itemCount = kTextRowCount;
    model.rowText = [this](int i) { return textRowName(i); };
    model.rowValue = [this](int i) { return textRowValue(i); };
  } else if (overlay == Overlay::Stats) {
    static constexpr StrId kStatsRowIds[] = {StrId::STR_STATS_SHOW_BOOK_STATS, StrId::STR_STATS_ALL_TIME,
                                             StrId::STR_STATS_READING_RHYTHM, StrId::STR_STATS_FINISHED_BOOKS};
    static_assert(std::size(kStatsRowIds) == kStatsPanelRows);
    model.panelTitle = tr(STR_READING_STATS);
    model.itemCount = kStatsPanelRows;
    model.rowText = [](int row) { return std::string(I18N.get(kStatsRowIds[row])); };
  } else {
    model.panelTitle = tr(STR_TOOL_MORE);
    model.itemCount = static_cast<int>(moreItems.size());
    model.rowText = [this](int i) { return moreRowName(i); };
    model.rowValue = [this](int i) { return moreRowValue(i); };
  }
  toolbarUi->setModel(model);
  toolbarUi->render();

  if (!mappedInput.hasTouch()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void EpubReaderActivity::handleOverlayInput() {
  if (!toolbarUi) return;

  // A modal option picker over the panel owns all input while open.
  if (overlayPopup.isActive()) {
    overlayPopup.handleInput(mappedInput, [this] {
      if (overlayPopup.isActive()) {
        paintOverlayPopup();  // highlight moved
        return;
      }
#if defined(CROSSPOINT_TTF_READER)
      // Family selection was applied in the popup callback; now the sheet is
      // back on top and the quick page-only relayout paints behind it. Focus
      // returns to the size row so +/- act on the next tap without the user
      // having to select that row again (issue #137).
      if (quickFontFamilyPending) {
        quickFontFamilyPending = false;
        quickFontRow = 0;
        renderQuickFontPage();
        return;
      }
#endif
      // Dismissed or selected: erase the dialog -- clean page back, then the
      // panel over it (the dialog can overhang the sheet onto the page).
      RenderLock lock;
      settleOverlayRefresh();
      if (overlayPageStored) {
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
        renderOverlay();
        pushOverlayRefresh();
      } else {
        requestUpdate();
      }
    });
    return;
  }
  const auto fastRedraw = [this] {
    RenderLock lock;  // the render task shares the framebuffer
    settleOverlayRefresh();
    renderOverlay();
    pushOverlayRefresh();
  };

  // Jump to another spine item (chapter scrub). The overlay stays up and is
  // re-drawn over the new page by renderBook().
  const auto gotoSpine = [this](int target) {
    const int spineCount = epub->getSpineItemsCount();
    target = std::clamp(target, 0, spineCount - 1);
    if (target != currentSpineIndex) {
      RenderLock lock;
      clearDeferredReposition();
      nextPageNumber = 0;
      currentSpineIndex = target;
      section.reset();
    }
    requestUpdate();
  };
  const auto toolOverlay = [](int tool) {
    if (tool == EpubReaderActivity::kToolContents) return Overlay::Contents;
    if (tool == EpubReaderActivity::kToolText) return Overlay::Text;
#ifdef READING_STATS_ENABLED
    // Tracking-off builds keep the tile visible but gated, like the classic
    // menu's dimmed row: opening the panel is refused, so BookStatsActivity
    // can never launch from the toolbar.
    if (tool == EpubReaderActivity::kToolStats && SETTINGS.shouldTrackReadingStats()) return Overlay::Stats;
#endif
    return Overlay::More;
  };

  // Touch first: FreeInkUI routes the frame against the tap targets the last
  // render registered and hands back the action it mapped to.
  const auto routed = toolbarUi->route(mappedInput);
#if defined(CROSSPOINT_TTF_READER)
  if (routed.routed) {
    LOG_DBG("ERS", "overlay=%d uiReady=%d routed event=%d value=%d", static_cast<int>(overlay),
            toolbarUi->routingReady(), static_cast<int>(routed.event), routed.value);
  }
#endif

#if defined(CROSSPOINT_TTF_READER)
  if (overlay == Overlay::FontSheet) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        closeFontSheet();
        return;
      case ReaderToolbarUi::Event::FontMinus:
        if (routed.value == 0) quickFontStep(-1);
        return;
      case ReaderToolbarUi::Event::FontPlus:
        if (routed.value == 0) quickFontStep(1);
        return;
      case ReaderToolbarUi::Event::FontRow:
        if (routed.value == 1) {
          quickFontSelectRow(1, false);
          openFontFamilyPicker();
        } else {
          quickFontSelectRow(0);
        }
        return;
      default:
        break;
    }
    if (routed.routed) return;

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeFontSheet();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      quickFontSelectRow(0);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      quickFontSelectRow(1);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (quickFontRow == 0) quickFontStep(-1);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (quickFontRow == 0) quickFontStep(1);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (quickFontRow == 1) {
        openFontFamilyPicker();
      } else {
        quickFontStep(1);
      }
      return;
    }
    return;
  }
#endif

  // --- Toolbar ---
  if (overlay == Overlay::Toolbar) {
    switch (routed.event) {
      case ReaderToolbarUi::Event::Dismiss:
        closeOverlayToPage();
        return;
      case ReaderToolbarUi::Event::Tool:
        focusedTool = routed.value;
        openOverlay(toolOverlay(focusedTool));
        return;
      case ReaderToolbarUi::Event::PrevChapter:
        gotoSpine(currentSpineIndex - 1);
        return;
      case ReaderToolbarUi::Event::NextChapter:
        gotoSpine(currentSpineIndex + 1);
        return;
      case ReaderToolbarUi::Event::Scrub:
        gotoSpine(static_cast<int>((static_cast<float>(routed.permille) / 1000.0f) *
                                       static_cast<float>(epub->getSpineItemsCount() - 1) +
                                   0.5f));
        return;
      default:
        break;
    }
    if (routed.routed) return;  // a touch frame the chrome consumed (or dead space)

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      closeOverlayToPage();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      focusedTool = (focusedTool + EpubReaderActivity::kToolTileCount - 1) % EpubReaderActivity::kToolTileCount;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      focusedTool = (focusedTool + 1) % EpubReaderActivity::kToolTileCount;
      panelCursorShown = true;
      fastRedraw();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openOverlay(toolOverlay(focusedTool));
      return;
    }
    const bool prev = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool next = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (prev || next) {
      gotoSpine(currentSpineIndex + (next ? 1 : -1));
    }
    return;
  }

  // --- Panels (Contents / Text / More) ---
  const int count = overlay == Overlay::Contents ? epub->getTocItemsCount()
                    : overlay == Overlay::Text   ? kTextRowCount
                    : overlay == Overlay::Stats  ? kStatsPanelRows
                                                 : static_cast<int>(moreItems.size());
  const int pageRows = std::max(1, toolbarUi->visibleRows());

  // Activate the highlighted row: change a value / jump to a chapter / run an
  // action. Shared by the Confirm button and a row tap.
  const auto activateRow = [this, count, &fastRedraw] {
    if (panelIndex < 0 || panelIndex >= count) return;
    if (overlay == Overlay::Text) {
      if (panelIndex == 0) {
#if defined(CROSSPOINT_TTF_READER)
        // Family and size live in the compact quick sheet; the full picker
        // remains available from the Settings text screen.
        openFontSheet();
#endif
      } else if (panelIndex == 4) {
        // Focus Reading is a genuine on/off: a tap toggles and applies live.
        SETTINGS.focusReadingEnabled = SETTINGS.focusReadingEnabled ? 0 : 1;
        applyTextSettingLive();
      } else {
        // Enum rows open the Settings-style option picker.
        showTextRowPopup(panelIndex);
      }
    } else if (overlay == Overlay::Contents) {
      const auto item = epub->getTocItem(panelIndex);
      if (item.spineIndex != -1) {
        RenderLock lock;
        clearDeferredReposition();
        currentSpineIndex = item.spineIndex;
        pendingAnchor = item.anchor;
        nextPageNumber = 0;
        section.reset();
      }
      overlay = Overlay::None;
      discardOverlayPage();
      requestUpdate();
    } else if (overlay == Overlay::Stats) {
#ifdef READING_STATS_ENABLED
      {
        recordCurrentPageReadingTime();
        BookReadingStats displayStats = stats;
        if (SETTINGS.shouldTrackReadingStats()) {
          displayStats.totalReadingSeconds += sessionReadingSeconds;
        }
        // The panel lists the real destinations directly; no intermediate
        // ReadingStatsMenuActivity hop.
        std::unique_ptr<Activity> target;
        if (panelIndex == 0) {
          target = makeUniqueNoThrow<BookStatsActivity>(
              renderer, mappedInput, epub->getTitle(), epub->getAuthor(), displayStats, epub->getCachePath(),
              epub->getCachePath().empty() ? GlobalReadingStats{} : GlobalReadingStats::load());
        } else if (panelIndex == 1) {
          target = makeUniqueNoThrow<GlobalStatsActivity>(renderer, mappedInput);
        } else if (panelIndex == 2) {
          target = makeUniqueNoThrow<ReadingRhythmActivity>(renderer, mappedInput);
        } else {
          target = makeUniqueNoThrow<FinishedBooksActivity>(renderer, mappedInput);
        }
        if (!target) {
          LOG_ERR("ERS", "OOM: reading stats screen");
          return;
        }
        overlay = Overlay::None;
        overlayPopup.dismiss();
        discardOverlayPage();
        startActivityForResult(std::move(target), [this](const ActivityResult& result) {
          if (epub && SETTINGS.shouldTrackReadingStats()) handleBookStatsReturn();
          if (std::holds_alternative<ClearPaceResult>(result.data) && epub) {
            stats.clearWpmStats();
            stats.save(epub->getCachePath());
          }
          requestUpdate();
        });
      }
#endif
    } else if (overlay == Overlay::More) {
      activateMoreRow(panelIndex);
    }
  };

  // Steps up to the toolbar -- the Back button and a tap on the page above
  // the sheet.
  const auto dismissPanel = [this, &fastRedraw] {
    overlay = Overlay::Toolbar;
    // Restore the snapshotted page under the toolbar instead of re-rendering
    // it (2+ refreshes -> one FAST). Re-store right away so another panel
    // round-trip can restore again.
    if (overlayPageStored) {
      {
        RenderLock lock;  // the render task shares the framebuffer
        settleOverlayRefresh();
        // No baseline resync: the glass shows the panel, and erasing it needs
        // the differential to keep diffing against the last pushed frame.
        renderer.restoreBwBuffer(/*resyncPanelBaseline=*/false);
        overlayPageStored = renderer.storeBwBuffer();
      }
      fastRedraw();  // takes its own RenderLock
      return;
    }
    requestUpdate();
  };

  // Pages the list by one screen of rows through the nav (measured page size,
  // no-op at the ends). A shown cursor rides along so the buttons continue
  // from what is visible; on touch boards only the viewport moves.
  const auto pageList = [this, count, pageRows, &fastRedraw](int direction) {
    if (count <= 0) return;
    const bool moved = toolbarUi->nav().scrollBy(direction * pageRows, count);
    if (panelCursorShown) {
      panelIndex = std::clamp(panelIndex + direction * pageRows, 0, count - 1);
      fastRedraw();
      return;
    }
    if (moved) fastRedraw();
  };

  switch (routed.event) {
    case ReaderToolbarUi::Event::Dismiss:
      dismissPanel();
      return;
    case ReaderToolbarUi::Event::Tool: {
      // Sheet-bottom tool switcher: hop straight to another panel.
      const Overlay target = toolOverlay(routed.value);
      if (target != overlay) {
        focusedTool = routed.value;
        openOverlay(target);
      }
      return;
    }
    case ReaderToolbarUi::Event::Row:
      // A tap on the right-edge strip pages the sheet instead (upper half =
      // previous page, lower half = next): swipes are unreliable on etched
      // glass, and a long contents list needs a fast way through.
      if (routed.x >= renderer.getScreenWidth() - 44) {
        pageList(routed.y >= renderer.getScreenHeight() - (renderer.getScreenHeight() * 62) / 200 ? 1 : -1);
        return;
      }
      panelIndex = routed.value;
      panelCursorShown = false;
      activateRow();
      return;
    default:
      break;
  }
  // Swipe up/down pages the list. Checked before the routed-frame return:
  // FUI routes every touch frame over the sheet, so a swipe's frames count as
  // routed (without dispatching -- too much travel for a tap) and the gesture
  // would otherwise never be seen.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    pageList(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return;
  }
  if (routed.routed) return;  // consumed by the chrome (title band, dead space)

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    dismissPanel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow();
    return;
  }

  // Up/Down (side) and Left/Right (front) move the cursor: a tap steps one
  // row, holding past PANEL_HOLD_MS jumps PANEL_HOLD_STEP rows in one go, which
  // is how you cross a hundreds-of-chapters contents list without a press per
  // row. The jump fires once on the hold and swallows the release that ends it,
  // so it never doubles up with the tap step.
  if (count > 0) {
    const bool up = mappedInput.isPressed(MappedInputManager::Button::Up) ||
                    mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool down = mappedInput.isPressed(MappedInputManager::Button::Down) ||
                      mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!panelHoldJumped && (up || down) && mappedInput.getHeldTime() >= PANEL_HOLD_MS) {
      const int step = down ? PANEL_HOLD_STEP : -PANEL_HOLD_STEP;
      panelIndex = std::clamp(panelIndex + step, 0, count - 1);
      panelHoldJumped = true;
      panelCursorShown = true;
      fastRedraw();
      return;
    }

    const bool releasedUp = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool releasedDown = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (releasedUp || releasedDown) {
      if (!panelHoldJumped) {
        panelIndex = releasedUp ? ButtonNavigator::previousIndex(panelIndex, count)
                                : ButtonNavigator::nextIndex(panelIndex, count);
        panelCursorShown = true;
        fastRedraw();
      }
      panelHoldJumped = false;
    }
  }
}

// First paint of the option picker over the panel (and highlight repaints).
// The dialog draws over the current framebuffer without clearing; erasing it
// on dismissal is the popup gate's restore in handleOverlayInput().
void EpubReaderActivity::paintOverlayPopup() {
  RenderLock lock;
  settleOverlayRefresh();
  overlayPopup.render(renderer);
  pushOverlayRefresh();
}

void EpubReaderActivity::applyReaderTextSettings() {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    SETTINGS.saveToFile();
    RenderLock lock;
    freeink::book::fontLoader.markDirty();
    // Reflow in place: drop the caches; the new generation produces a fresh
    // build and the position restores through the page's char offset.
    ttfInvalidateCaches();
    return;
  }
#endif
  SETTINGS.saveToFile();
  // (Re)load or unload the selected SD-card font for the current family/size.
  // The reader otherwise only loads SD fonts on book open, so without this an
  // in-reader font change wouldn't take effect until re-opening the book.
  sdFontSystem.ensureLoaded(renderer);
  RenderLock lock;
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }
  section.reset();  // force re-pagination with the new settings
}

// The More panel carries everything the classic list menu offers except the
// entries with their own surface: chapters -> Contents tool, text -> Text
// tool, per-book stats -> its own tool tile (stats build only).
void EpubReaderActivity::buildMoreActions() {
  using MA = EpubReaderMenuActivity::MenuAction;
#ifdef READING_STATS_ENABLED
  EpubReaderMenuActivity::buildToolbarMoreItems(moreItems, !currentPageFootnotes.empty(), !cachedBookmarks.empty());
#else
  EpubReaderMenuActivity::buildMenuItems(moreItems, !currentPageFootnotes.empty(), !cachedBookmarks.empty());
  moreItems.erase(std::remove_if(moreItems.begin(), moreItems.end(),
                                 [](const auto& item) {
                                   return item.action == MA::SELECT_CHAPTER || item.action == MA::TEXT_SETTINGS;
                                 }),
                  moreItems.end());
#endif
}

std::string EpubReaderActivity::moreRowName(int row) const {
  return row >= 0 && row < static_cast<int>(moreItems.size()) ? I18N.get(moreItems[row].labelId) : "";
}

std::string EpubReaderActivity::moreRowValue(int row) const {
  using MA = EpubReaderMenuActivity::MenuAction;
  static constexpr StrId kOrient[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_ORIENTATION_INVERTED,
                                      StrId::STR_LANDSCAPE_CCW};
  static_assert(std::size(kOrient) == CrossPointSettings::ORIENTATION_COUNT, "orientation labels");
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return "";
  switch (moreItems[row].action) {
    case MA::ROTATE_SCREEN:
      return I18N.get(kOrient[SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT]);
    case MA::AUTO_PAGE_TURN:
      return (autoTurnOption == 0 || autoTurnOption >= static_cast<int>(std::size(PAGE_TURN_RATES)))
                 ? std::string(tr(STR_STATE_OFF))
                 : std::to_string(PAGE_TURN_RATES[autoTurnOption]);
    case MA::NIGHT_MODE:
      return SETTINGS.screenInverted ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case MA::FRONTLIGHT:
      return Frontlight.isOn() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    default:
      return "";
  }
}

void EpubReaderActivity::activateMoreRow(int row) {
  using MA = EpubReaderMenuActivity::MenuAction;
  if (row < 0 || row >= static_cast<int>(moreItems.size())) return;
  const auto action = moreItems[row].action;
  {
    RenderLock lock;  // several actions launch screens that paint the framebuffer
    settleOverlayRefresh();
  }
  // In-place toggles keep the panel open and re-render the page beneath it.
  switch (action) {
    case MA::ROTATE_SCREEN: {
      static constexpr StrId kOrientIds[] = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW,
                                             StrId::STR_ORIENTATION_INVERTED, StrId::STR_LANDSCAPE_CCW};
      static_assert(std::size(kOrientIds) == CrossPointSettings::ORIENTATION_COUNT, "orientation options");
      overlayPopup.show(StrId::STR_ORIENTATION, kOrientIds, static_cast<int>(std::size(kOrientIds)),
                        SETTINGS.orientation % CrossPointSettings::ORIENTATION_COUNT, [this](int idx) {
                          if (idx == SETTINGS.orientation) return;
                          applyOrientation(static_cast<uint8_t>(idx));
                          // The stored page is laid out for the old orientation.
                          discardOverlayPage();
                          requestUpdate();
                        });
      paintOverlayPopup();
      return;
    }
    case MA::AUTO_PAGE_TURN: {
      std::vector<std::string> labels;
      labels.reserve(std::size(PAGE_TURN_RATES));
      labels.emplace_back(tr(STR_STATE_OFF));
      for (size_t i = 1; i < std::size(PAGE_TURN_RATES); ++i) labels.push_back(std::to_string(PAGE_TURN_RATES[i]));
      overlayPopup.show(StrId::STR_AUTO_TURN_PAGES_PER_MIN, labels, autoTurnOption, [this](int idx) {
        autoTurnOption = idx;
        toggleAutoPageTurn(static_cast<uint8_t>(idx));
      });
      paintOverlayPopup();
      return;
    }
    case MA::NIGHT_MODE:
      SETTINGS.screenInverted = SETTINGS.screenInverted == 0 ? 1 : 0;
      SETTINGS.saveToFile();
      discardOverlayPage();
      requestUpdate();
      return;
    case MA::FRONTLIGHT: {
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      SETTINGS.saveToFile();
      {
        RenderLock lock;  // the render task shares the framebuffer
        renderOverlay();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      }
      return;
    }
    default:
      break;
  }
  // Leaf actions open their own screen / perform the action; close the overlay first.
  overlay = Overlay::None;
  discardOverlayPage();
  if (action == MA::TOGGLE_BOOKMARK) {
    // No child activity here to trigger the re-render the list menu relies on:
    // show the same confirmation popup the long-press path does.
    addBookmark();
    showBookmarkMessage = true;
    bookmarkMessageTime = millis();
    requestUpdate();
    return;
  }
  onReaderMenuConfirm(action);
  // Actions that neither open a screen nor leave the reader (a sync with no
  // credentials, say) would otherwise leave the closed panel on screen.
  if (action != MA::GO_HOME && action != MA::DELETE_CACHE) requestUpdate();
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  if (savePosition && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
#if defined(CROSSPOINT_TTF_READER)
    // TTF mirror: ttfPage is the chapter-local page the origin save maps
    // through (page-anchored restore, §3.5 item 8).
    const int savedPage = ttf_ ? ttfPage : (section ? section->currentPage : 0);
#else
    const int savedPage = section ? section->currentPage : 0;
#endif
    savedPositions[footnoteDepth] = {currentSpineIndex, savedPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, savedPage);
  }

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
  int targetSpineIndex = sameFile ? currentSpineIndex : epub->resolveHrefToSpineIndex(hrefStr);

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;
    return;
  }

  {
    RenderLock lock;
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    if (!epub || ttfPage < 0 || ttfPage >= static_cast<int>(ttfPageCount)) return;
    LOG_DBG("ERS", "Toggle TTF bookmark at spine %d, page %d", currentSpineIndex, ttfPage);
    const int currentPage = ttfPage;
    const int pageCount = static_cast<int>(ttfPageCount);
    // Page-text summary from the cached page runs (cold path).
    std::string pageText;
    {
      RenderLock lock;  // render task owns/uses the scratch arena
      const size_t mark = ttf_->scratch().mark();
      freeink::book::Page page{};
      if (ttf_->readPage(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(currentPage), &page)) {
        for (uint16_t r = 0; r < page.runCount; ++r) {
          pageText.append(page.runs[r].text, page.runs[r].len);
        }
      }
      ttf_->scratch().release(mark);
    }
    SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
    const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);
    const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
    cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                         [&](const BookmarkEntry& b) {
                                           return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                          pageRange);
                                         }),
                          cachedBookmarks.end());
    if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
      bookmarkRemoved = true;
      currentPageBookmarked = false;
    } else {
      // Bookmarks keep spine + page under the TTF path (no visible-text
      // offset substrate in v1, §3.5).
      BookmarkEntry entry;
      entry.percentage = progress.percentage;
      entry.xpath = progress.xpath;
      entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
      entry.computedSpineIndex = currentSpineIndex;
      entry.computedChapterPageCount = pageCount;
      entry.computedChapterProgress = currentPage;
      cachedBookmarks.insert(cachedBookmarks.begin(), entry);
      bookmarkRemoved = false;
      currentPageBookmarked = true;
    }
    if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
      LOG_ERR("ERS", "Failed to save bookmarks");
    }
    requestUpdate();
    return;
  }
#endif
  if (!section || !epub) return;
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock;
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    const std::optional<uint32_t> offset =
        currentPageVisibleOffset.has_value() ? currentPageVisibleOffset
        : (currentPage >= 0 && currentPage < section->pageCount)
            ? section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))
            : std::nullopt;
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    if (!epub || cachedBookmarks.empty() || ttfPage < 0 || ttfPageCount == 0) {
      currentPageBookmarked = false;
      return;
    }
    const int pageCount = static_cast<int>(ttfPageCount);
    const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, ttfPage, pageCount);
    currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
      return bookmarkMatchesProgress(b, currentSpineIndex, ttfPage, pageCount, pageRange);
    });
    return;
  }
#endif
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
#if defined(CROSSPOINT_TTF_READER)
  if (ttf_) {
    info.currentPage = ttfPage + 1;
    info.totalPages = static_cast<int>(ttfPageCount);
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0 && ttfPage >= 0) {
      const float chapterProgress = static_cast<float>(ttfPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
    return info;
  }
#endif
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  localPos.hasResolvedSpineIndex = true;
  localPos.hasMappedPage = true;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}

#if FREEINK_CAP_FRONTLIGHT
bool EpubReaderActivity::handleSideSwipeFrontlight() {
  // Continuous drag tracking for pixel-precision frontlight control.
  // The SDK's wasSwipe/decodeSwipe requires 60px minimum travel before it
  // fires, which maps to ~7.5% on an 800px-wide screen (in landscape) —
  // too coarse for night-time fine tuning. Instead we track the touch live:
  // wasScreenTouchDown starts a drag on a side edge, isScreenTouchHeld
  // reports incremental Y deltas each frame, and wasScreenTouchReleased
  // ends the drag.
  //
  // Sensitivity: PIXELS_PER_PERCENT controls how many pixels of vertical
  // travel equal 1% frontlight change. At 3px/1%, a full 0-100% range
  // needs ~300px of drag (~63% of a 480px screen in landscape), giving
  // enough travel to avoid twitchy accidental max-outs while keeping
  // 1% precision.
  static constexpr int PIXELS_PER_PERCENT = 3;

  const int screenW = renderer.getScreenWidth();
  static constexpr float SIDE_BAND = 0.08f;  // 8% of width from each edge
  const int leftBand = static_cast<int>(screenW * SIDE_BAND);
  const int rightBand = screenW - static_cast<int>(screenW * SIDE_BAND);

  // --- Touch-down: start a frontlight drag on a side edge -------------------
  if (!frontlightDrag.active) {
    int tx = 0;
    int ty = 0;
    if (!mappedInput.wasScreenTouchDown(tx, ty)) return false;
    if (tx < leftBand) {
      frontlightDrag.active = true;
      frontlightDrag.leftSide = true;
      frontlightDrag.touchStartY = ty;
    } else if (tx >= rightBand) {
      frontlightDrag.active = true;
      frontlightDrag.leftSide = false;
      frontlightDrag.touchStartY = ty;
    }
    return frontlightDrag.active;  // true if we started a drag, false otherwise
  }

  // --- Drag in progress: apply incremental Y delta ---------------------------
  int cx = 0;
  int cy = 0;
  if (mappedInput.isScreenTouchHeld(cx, cy)) {
    // Convert pixel delta to percentage: PIXELS_PER_PERCENT=3 means 3px of
    // vertical travel = 1% change; ~300px for the full 0→100% range.
    const int deltaY = cy - frontlightDrag.touchStartY;
    const int up = deltaY < 0;  // dy < 0 = finger moved up
    const int step = std::abs(deltaY) / PIXELS_PER_PERCENT;
    frontlightDrag.touchStartY = cy;  // reset baseline for next frame

    if (step == 0) return true;  // no movement this frame

    if (frontlightDrag.leftSide) {
      // Left edge: color temperature. Up = warmer.
      if (!Frontlight.hasColorTemperature()) return true;  // consumed, no change
      int next = static_cast<int>(SETTINGS.frontlightWarmth) + (up ? step : -step);
      next = std::clamp(next, 0, 100);
      if (next != static_cast<int>(SETTINGS.frontlightWarmth)) {
        SETTINGS.frontlightWarmth = static_cast<uint8_t>(next);
        Frontlight.setWarmth(SETTINGS.frontlightWarmth);
      }
    } else {
      // Right edge: brightness. Up = brighter, down = dimmer.
      // Sliding all the way down (to 0) turns the light off.
      int next = static_cast<int>(SETTINGS.frontlightBrightness) + (up ? step : -step);
      if (next <= 0) {
        // Turn the light off. Do NOT reset lastBrightness (keep the pre-off
        // value) — mirrors FrontlightPanelActivity::toggleLight, which calls
        // only setOn(false) so the panel slider and swipe both restore to the
        // same brightness when the light is turned back on.
        SETTINGS.frontlightOn = 0;
        Frontlight.setOn(false);
      } else {
        int clamped = std::clamp(next, static_cast<int>(FRONTLIGHT_MIN_BRIGHTNESS), 100);
        if (static_cast<int>(SETTINGS.frontlightBrightness) != clamped) {
          SETTINGS.frontlightBrightness = static_cast<uint8_t>(clamped);
          if (!SETTINGS.frontlightOn) {
            SETTINGS.frontlightOn = 1;
            Frontlight.setOn(true);
          }
          Frontlight.setBrightness(SETTINGS.frontlightBrightness);
        }
      }
    }
    return true;  // consumed — prevents page turn on the same frame
  }

  // --- Release: end the drag (no re-render needed; hardware already updated)
  if (mappedInput.wasScreenTouchReleased()) {
    frontlightDrag.active = false;
  }
  return true;
}
#endif
