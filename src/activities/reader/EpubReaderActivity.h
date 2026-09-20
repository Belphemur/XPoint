#pragma once

#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/PageLink.h>
#include <Epub/Section.h>
#include <Memory.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "BookmarkEntry.h"
#include "ChapterPosition.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressManager.h"
#include "ProgressMapper.h"
#include "ReaderActivity.h"
#include "ReaderToolbarUi.h"
#include "TouchLongPressMode.h"
#include "components/OptionPopup.h"
#if defined(CROSSPOINT_TTF_READER)
#include "FibpPrefetchWorker.h"
#include "QuickPageCapture.h"
#include "TtfBookRuntime.h"
#endif
#ifdef READING_STATS_ENABLED
#include "BookReadingStats.h"
#include "GlobalReadingStats.h"
#include "ReadingStatsUtils.h"
#endif

class EpubReaderActivity final : public ReaderActivity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  std::string pendingAnchor;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  std::optional<uint32_t> currentPageVisibleOffset;
  std::optional<uint32_t> pendingOffsetJump;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  int8_t pendingManualTurn = 0;
  bool pendingPercentJump = false;
  float pendingSpineProgress = 0.0f;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool showDictionaryMessage = false;
  // When set, the dictionary popup shows the TTF-path "not available" string
  // instead of the no-dictionary one (kody, PR #113).
  bool dictionaryMessageTtf = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool currentPageBookmarked = false;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  std::unique_ptr<Section> nextSectionPrefetch = nullptr;
  int nextSectionSpineIndex = -1;
  unsigned long lastRenderCompleteMs = 0;
  bool bookmarkRemoved = false;
  std::vector<BookmarkEntry> cachedBookmarks;
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingReadFolderMove = false;

  // Toolbar reader menu (SETTINGS.readerMenuStyle == READER_MENU_TOOLBAR): drawn
  // over the page instead of pushing the full-screen list menu. Select opens the
  // Toolbar; its tools open the Contents/Text/More bottom-sheet panels.
  enum class Overlay { None, Toolbar, Contents, Text, More, Stats, FontSheet };
  // Toolbar tool focus. 0=Contents, 1=Text, then the stats tile
  // (READING_STATS_ENABLED only) and More last — kTool* + kToolTileCount are
  // the source of truth, never bare literals.
  static constexpr int kToolContents = 0;
  static constexpr int kToolText = 1;
  static constexpr int kToolStats = 2;
  static constexpr int kToolMore =
#ifdef READING_STATS_ENABLED
      3;
#else
      2;
#endif
  // Number of tiles in the tool row (kToolMore is the last one).
  static constexpr int kToolTileCount = kToolMore + 1;
  // Stats panel rows: This Book / All Books / Reading Rhythm / Finished Books.
  static constexpr int kStatsPanelRows = 4;
  Overlay overlay = Overlay::None;
  int focusedTool = 0;  // toolbar tool focus: kToolContents..kToolMore
  int panelIndex = 0;   // selected row within the active panel
  // Panel list navigation: a tap steps one row, a hold jumps PANEL_HOLD_STEP rows in one go
  // (a contents list runs to hundreds of chapters). One jump per hold, not a repeat -- every
  // step repaints the panel, so repeating is bounded by the e-ink refresh anyway and reads as
  // sluggish. True once a hold has jumped, so the release that ends it is swallowed.
  static constexpr unsigned long PANEL_HOLD_MS = 1500;
  static constexpr int PANEL_HOLD_STEP = 10;
  bool panelHoldJumped = false;
  // Whether the panel draws its cursor row. Button boards always do; touch
  // boards only once a button has moved it, so a tapped row is not left inverted.
  bool panelCursorShown = false;
  // FreeInkUI chrome + tap targets for the overlay; created when it opens,
  // released when it closes.
  std::unique_ptr<ReaderToolbarUi> toolbarUi;
  // Modal option picker over the panel (same component the Settings screens
  // use), for enum rows: font family / size / line spacing / alignment /
  // orientation / auto page turn. Capacity 33 covers built-in + the TTF
  // scanner's 32-family cap; only the popup's visible page has touch targets.
  // Toggle rows stay one-tap toggles, as in Settings.
  OptionPopup<33, 8> overlayPopup;
#if defined(CROSSPOINT_TTF_READER)
  // Quick font sheet focus: 0 = size, 1 = family. Size applies each +/- step
  // to the displayed page before pushing a FAST refresh; family opens the
  // modal picker. Caches are invalidated only when the sheet closes (SD write
  // + full reflow are deliberately not per-tap costs).
  int quickFontRow = 0;
  bool quickFontFamilyPending = false;
  // Paint-once preview state: the capture buffer is allocated on the first
  // quick relayout and lives until the sheet closes, so taps never churn
  // the PSRAM pool. The capture carries the candidate page out of the
  // engine's per-page arena for the single post-scan paint.
  QuickPageCapture quickFontPreview;
  PoolBytes quickFontPreviewBuf;
  void openFontSheet();
  void openFontFamilyPicker();
  void quickFontStep(int direction);
  void quickFontSelectRow(int row, bool refresh = true);
  void renderQuickFontPage();
  void closeFontSheet();
#endif
  // True while a clean-page snapshot (renderer.storeBwBuffer) backs the open
  // overlay, letting panel->toolbar steps restore the page without a full
  // re-render. Discarded on close / whenever the page under the overlay changes.
  bool overlayPageStored = false;
  // True while a deferred overlay chrome refresh (pushOverlayRefresh) may still
  // be running on the panel. settleOverlayRefresh() must run before the
  // framebuffer is touched or another differential refresh is pushed. Atomic:
  // written by the render/loop tasks and read in ~EpubReaderActivity().
  std::atomic<bool> overlayRefreshPending{false};
  // Set when a settle gave up after OVERLAY_REFRESH_SETTLE_TIMEOUT_MS: the
  // panel is wedged, so later settles only single-check refreshBusy() instead
  // of re-polling the full timeout, and blocking driver calls are skipped.
  // Only touched under the RenderLock, hence not atomic.
  bool overlaySettleTimedOut = false;
  // Throttle for retryDeferredOverlayPush()'s non-blocking re-checks.
  uint32_t overlayPushRetryMs = 0;
  void pushOverlayRefresh();
  void settleOverlayRefresh();
  void retryDeferredOverlayPush();
  int autoTurnOption = 0;  // current auto page-turn rate index (More panel)
  std::vector<EpubReaderMenuActivity::MenuItem> moreItems;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  std::vector<PageLink> currentPageLinks;
  int currentPageLinkMarginLeft = 0;
  int currentPageLinkMarginTop = 0;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

#ifdef READING_STATS_ENABLED
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  uint32_t sessionReadingSeconds = 0;
  // Forward page turns completed this session; the Avg Session window only
  // records sessions with real engagement (SESSION_MIN_PAGE_TURNS), not a
  // book merely left open.
  uint16_t sessionPageTurns = 0;
  unsigned long pageShownAtMs = 0UL;
  // Words on the page currently rendered, cached at render time for the pace
  // sample taken on the next forward turn.
  uint16_t currentPageWordsOnPage = 0;

  bool currentPageReadingSecondsForStats(uint32_t& seconds) const;
  void recordCurrentPageReadingTime();
  void recordForwardPagePaceSample(uint32_t seconds, uint16_t wordsOnPage);
  // Completion/achievement flow: real end auto-completes, while a final-page
  // or 100% exit asks first and latches a declined prompt.
  void onGoHomeRequested() override;
  void setBookCompleted(bool completed);
  void goHomeOrShowCompletionAchievement();
  // Imports only user-editable completion state saved by BookStatsActivity;
  // live session counters stay authoritative in memory.
  void applyBookStatsEditsFromDisk();
  void syncFinishedBookIndex();
  void handleBookStatsReturn();
  BookReadingStats achievementStatsPreview(uint32_t* pendingReadingSeconds = nullptr) const;
#endif

  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  bool partialRebuildStartFailed = false;

#if defined(CROSSPOINT_TTF_READER)
  // Native-TTF page source (design §3.5). Null until loadBook() succeeds
  // (a failed open falls back to the legacy Section path — build-flag kill
  // switch). The legacy section members above stay the single position
  // mirror for chrome: nextPageNumber/cachedChapterTotalPageCount are kept
  // in sync with the TTF page state so renderStatusBar/KOReader/bookmarks
  // read the same values on both paths.
  std::unique_ptr<freeink::book::TtfBookRuntime> ttf_;
  // Index-ahead prefetch worker (R1): transient, per book. Created lazily on
  // the first TTF render (generation known), cancelled in onExit(). Inert on
  // non-PSRAM/stb builds (stub).
  std::unique_ptr<freeink::book::FibpPrefetchWorker> fibpWorker_;
  bool fibpBegun_ = false;
  char fibpFamily_[48] = {};  // family the worker's faces were built for
  uint32_t fibpNotifiedGen_ = 0;
  bool fibpDeferred_ = false;           // a build is delegated to the worker
  unsigned long fibpDeferPollMs_ = 0;   // last deferred-build poll (throttle)
  unsigned long fibpDeferStartMs_ = 0;  // when the current delegation began
  // Soak addendum: the opened chapter's partial completion handed to the
  // worker (requestResumeClaim) while the reader paints from the partial.
  int16_t fibpResumeClaimedSpine_ = -1;
  bool fibpResumeSeenBuilding_ = false;  // worker observed holding the claim
  unsigned long fibpResumeRequestMs_ = 0;
  int ttfSpine = -1;          // spine the runtime's reader/session belong to
  int ttfPage = 0;            // chapter-local page index
  uint32_t ttfPageCount = 0;  // pages available for the current chapter
  // Estimated chapter total while a build is in flight: pages built scaled
  // by the layout session's input progress (the old Section engine's
  // extrapolation, owner steer: return after a few pages and let the user
  // read; the count refines as indexing progresses).
  uint32_t ttfEstimatedPageCount() const;
  uint32_t ttfGeneration = 0;
  bool ttfGenerationValid = false;
  bool ttfRestoreLastPage = false;                  // back-navigation into the previous chapter
  std::atomic<bool> ttfFrameRenderComplete{false};  // framebuffer holds the last rendered page
  uint32_t ttfCurrentCharStart = 0;                 // charStart of the last rendered page
  // Progress-record restore data (consumed on the chapter's first open).
  bool ttfHasSavedPosition = false;
  uint16_t ttfSavedSpine = 0;
  uint32_t ttfSavedCharOffset = 0;
  uint32_t ttfSavedGeneration = 0;
  bool ttfPrefetchActive = false;     // session building the NEXT spine
  bool ttfReflowJumpPending = false;  // position restore via char offset
  void renderBookTtf();
  // Grayscale base refresh shared by both TTF gray transports: cleanup cycle
  // when due, otherwise the grayscale base waveform (§11 Q7).
  void ttfDisplayGrayBase();
  // Gray-parity transports (§11 Q7): strips where the panel supports them,
  // full-frame plane buffers otherwise (UC8279 X4). Both share the base/
  // cleanup contract; scratchMark releases after the caller's arena walk.
  void renderTtfGrayStrips(const freeink::book::Page& page, const freeink::book::LayoutParams& params,
                           size_t scratchMark);
  void renderTtfGrayFullFrame(const freeink::book::Page& page, const freeink::book::LayoutParams& params,
                              size_t scratchMark);
  // Rasterizes an engine page through the active render mode (gray parity or
  // 1bpp): text + rubies + rules + images/placeholder. Shared by the reader's
  // own page render and the TTF dictionary selector's repaint hook.
  void paintTtfPage(const freeink::book::Page& page, void* font);
  // Render hook handed to DictionaryWordSelectActivity: repaints the page the
  // selector was opened on (same spine/page members, reader frozen beneath).
  static void renderTtfSelectorPage(void* ctx, GfxRenderer& renderer);
  bool ttfResolveTargetPage(int& targetOut, const freeink::book::LayoutParams& params, bool& needFullBuild);
  void ttfBackgroundBuildTick();
  void ttfPrefetchTick();
  // Index-ahead worker wiring (R3/R4): (re)spawn/notify on the current
  // generation and chapter, stop on exit. Single-writer handoff: while the
  // worker builds the entered chapter, the sync path defers to its commit.
  void updateFibpWorker(uint32_t generation, const freeink::book::LayoutParams& params);
  void stopFibpWorker();
  void ttfInvalidateCaches();
  void ttfShowIndexingPopup();
  void ttfSaveProgress();
  void finishTtfPageRender();
  bool ttfPageTurn(bool isForwardTurn);
  // P4 one-page-ahead prerender (port of upstream 60079923, adapted to the
  // TTF engine): after a fully committed page-N frame, paint page N+1's
  // content-only (no status bar, no flush) into the framebuffer so the next
  // FORWARD turn skips layout+build+base-paint entirely. The prerender is
  // one RenderLock-protected transaction (renderBookTtf's prerender pass)
  // that starts only after the previous pass's waitRefreshComplete(); ready
  // is published only after the paint. Any other render pass invalidates it.
  // See the review contract in docs/design/2026-09-19-freetype-render-cache.md:
  // it is also invalidated (or the current page re-rendered) on any full-FB
  // flush outside the forward turn's own commit (deferred overlay pushes,
  // popup chrome opens).
  struct TtfPreRenderedPage {
    bool ready = false;
    int16_t spineIndex = -1;
    int16_t pageIndex = -1;
    // Validity axes the consume guard must match (soak finding #3: the
    // framebuffer content is only the target page when ALL of these are
    // unchanged since the prerender painted it).
    uint32_t generation = 0;     // layoutGenerationHash at prerender time
    uint8_t orientation = 0xFF;  // GfxRenderer::Orientation snapshot
    bool monochrome = false;     // effective text raster mode snapshot
    // Chunked-pump state (soak finding #5): the prerender paints in ≤
    // kPreRenderSliceBudgetMs slices, one per loop tick, so a button press
    // is never more than one slice away from being served. pumping marks an
    // unfinished paint (the framebuffer holds a PARTIAL page — never
    // consumable); nextRun/nextChar is the paint cursor across slices
    // (run index; nextChar = intra-run glyph offset — review r5).
    bool pumping = false;
    uint16_t nextRun = 0;
    uint32_t nextChar = 0;
    uint8_t slicesUsed = 0;
  };
  TtfPreRenderedPage ttfPreRendered;
  // millis() at renderBookTtf entry — busy-window telemetry (soak finding
  // #3 P2): input is polled, so presses fully inside a long render window
  // are dropped; the window length must be visible in the soak log.
  uint32_t ttfRenderBusyStartMs = 0;
  // Set by finishTtfPageRender() after a normal page render to request the
  // prerender pass; consumed and cleared by renderBookTtf() before any state
  // checks. Never set while an overlay/deferred overlay refresh is live.
  bool ttfPendingPreRender = false;
  // Set by the ttfPageTurn() fast path to tell renderBookTtf() the frame
  // buffer already holds the next page's content: only the status bar and
  // the display commit are needed.
  bool ttfUsePreRenderedBuffer = false;
  // True when finishTtfPageRender() painted a chrome popup this pass — such
  // a frame must stay page N's context, so the prerender stays unscheduled.
  bool ttfChromePopupShown = false;
  void ttfInvalidatePreRender(const char* reason);
  // Soak triage: log a fast-path miss reason once per distinct reason.
  void ttfLogPrerenderMiss(const char* reason);
  // The ACTIVE text raster mode (degrade-aware snapshot axis).
  bool ttfEffectiveMonochromeSnapshot() const;
  void ttfSchedulePreRender();
  // Both passes re-derive their target from live state (the flags only say a
  // pass was requested) and need the layout params for the paint font.
  void ttfRunPreRenderPass(const freeink::book::LayoutParams& params);
  bool ttfFastDisplayPass(const freeink::book::LayoutParams& params);
  // Extracts the engine page's internal (resolvable) footnote links into
  // currentPageFootnotes; shared by the normal and fast-display passes.
  void ttfExtractFootnotes(const freeink::book::Page& page);
  // Display commit + finalize tail shared by the normal and fast-display
  // passes: gray-route dispatch (strips/full-frame), 1bpp fallback with the
  // async-overlap window, telemetry, frame-complete publication, progress
  // save, chrome popups, and the prerender scheduling hook.
  void ttfCommitFrame(const freeink::book::Page& page, const freeink::book::LayoutParams& params, size_t scratchMark);
#endif

  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  static constexpr size_t BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024;
  static constexpr size_t BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024;
  bool buildTickHeapGate();
  bool buildHeapPaused = false;
  void prefetchNextChapterDuringDisplay();
  static constexpr size_t RENDER_MIN_FREE_HEAP = 24 * 1024;
  // Chunked prerender slice budget (soak finding #5): one prerender slice per
  // loop tick, so a button press is served at most one slice after it lands
  // (the loop processes input before the render pass runs).
  static constexpr uint32_t kPreRenderSliceBudgetMs = 120;
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  bool buildPopupPending = false;
  void showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh);
  bool applyDeferredReposition();
  void clearDeferredReposition();
  void rememberCurrentContentOffset();
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Live section position, or the values cached before a child screen
  // released the section.
  ChapterPosition chapterPosition() const;
  int bookPercentFor(const ChapterPosition& position) const;
  void openReaderMenu();
  // Toolbar reader menu (see Overlay above).
  bool usesToolbarMenu() const;
  // True while any reader chrome is up (toolbar sheet, a panel, or a popup
  // over a panel). The Home-key "Go Back" shortcut / home gesture close it
  // instead of leaving the book.
  bool isChromeOpen() const;
  bool handleHomeGesture() override;
  void openOverlay(Overlay target);
  void closeOverlayToPage();
  void discardOverlayPage();
  void handleOverlayInput();
  void renderOverlay();
  std::string currentChapterTitle() const;
  // Text panel rows (font, size, line spacing, alignment, focus reading).
  std::string textRowName(int row) const;
  std::string textRowValue(int row) const;
  void showTextRowPopup(int row);
  // Persist + re-paginate + re-render under the open panel (live preview).
  void applyTextSettingLive();
  void paintOverlayPopup();
  // Persist the reader text settings, (re)load the selected SD font, and
  // re-paginate the current chapter so changes apply without re-opening the book.
  void applyReaderTextSettings();
  // More panel rows.
  void buildMoreActions();
  std::string moreRowName(int row) const;
  std::string moreRowValue(int row) const;
  void activateMoreRow(int row);
  // TouchLongPressMode (see TouchLongPressMode.h) is passed to
  // openDictionaryWordSelect so the activity never reads the mutable
  // SETTINGS.touchLongPressAction global.
  void openDictionaryWordSelect(int touchX = -1, int touchY = -1,
                                TouchLongPressMode mode = TouchLongPressMode::Dictionary);
  // Frontlight side-swipe gestures: left edge adjusts color temperature,
  // right edge adjusts brightness. Independent of touchReaderControls.
  // Gated by FREEINK_CAP_FRONTLIGHT (compile-time) + settings toggle (runtime).
  // Returns true when a side-swipe was consumed (skips page-turn for this frame).
  //
  // Uses continuous touch tracking (not wasSideSwipe/decodeSwipe) for 1%
  // precision: the SDK's swipe recognizer requires 60px minimum travel before
  // it fires, which maps to ~4% on a 1448px screen — too coarse for night-time
  // fine adjustment. Instead, drag state is tracked across loop() calls via
  // wasScreenTouchDown + isScreenTouchHeld + wasScreenTouchReleased.
  // Returns true when a side-swipe was consumed (skips page-turn for this frame).
#if FREEINK_CAP_FRONTLIGHT
  bool handleSideSwipeFrontlight();
#endif
  bool launchKOReaderSync();
  unsigned long confirmLongPressThreshold() const;
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void applyOrientation(uint8_t orientation);
  void applyInitialOrientation() override;
  // The orientation the current layout was built for. The control center's
  // orientation tile can move SETTINGS.orientation while this reader sits on
  // the activity stack, and Pop restores it without onEnter(), so the drift has
  // to be noticed here rather than assumed away.
  uint8_t appliedOrientation = 0;

#if FREEINK_CAP_FRONTLIGHT
  // Continuous frontlight drag state. Updated by handleSideSwipeFrontlight()
  // across loop() frames: wasScreenTouchDown starts the drag, isScreenTouchHeld
  // reports incremental deltas each frame, wasScreenTouchReleased ends it.
  struct FrontlightDragState {
    bool active = false;
    bool leftSide = false;  // true = warmth edge, false = brightness edge
    int touchStartY = 0;    // logical screen Y where the drag began
  };
  FrontlightDragState frontlightDrag;
#endif

  bool loadBook() override;
  std::string getBookTitle() const override { return epub ? epub->getTitle() : ""; }
  std::string getBookAuthor() const override { return epub ? epub->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return epub ? epub->getThumbBmpPath() : ""; }
  void renderBook() override;
  void onEndOfBookRendered() override;

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh)
      : ReaderActivity("EpubReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~EpubReaderActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool skipLoopDelay() override;

  // True while a background build is live (FIBP worker indexing, TTF session,
  // or a section build): keeps the CPU out of low-power — the input-idle
  // governor would otherwise drop to LOW_POWER_FREQ mid-index (measured:
  // 2.6 s/page spine builds and a task-WDT IDLE0 starvation reboot) — and
  // holds the auto-sleep timer until the work drains. The worker self-exits
  // once every spine has been attempted, so this is bounded.
  bool preventAutoSleep() override;

  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
