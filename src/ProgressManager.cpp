#include "ProgressManager.h"

#include <Logging.h>

#include <string>

#include "activities/reader/ProgressFile.h"

namespace {
// Low priority: progress persistence must never compete with rendering or
// input. Pinned to core 0 on dual-core boards (the render task owns core 1,
// ActivityManager.cpp) so the flusher cannot time-slice with a render; on
// single-core boards priority 1 keeps it behind the render task at the same
// level only when the render task blocks.
// NOTE: this ESP-IDF build passes usStackDepth in BYTES (the render task at
// ActivityManager.cpp passes 8192 directly), not FreeRTOS words.
constexpr UBaseType_t PROGRESS_TASK_PRIORITY = 1;
constexpr size_t SAVER_STACK_BYTES = 2048;
constexpr char PROGRESS_TASK_NAME[] = "progress_mgr";
}  // namespace

ProgressFlush::Record makeRecord(uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount, bool hasOffset,
                                 uint32_t visibleTextOffset) {
  ProgressFlush::Record record;
  record.spineIndex = spineIndex;
  record.pageNumber = pageNumber;
  record.pageCount = pageCount;
  record.hasOffset = hasOffset;
  record.visibleTextOffset = visibleTextOffset;
  return record;
}

ProgressManager progressManager;

void ProgressManager::begin() {
  if (mutex_ == nullptr) {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
      LOG_ERR("PRG", "OOM: progress manager mutex");
      return;
    }
  }
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t saverTaskCore = 0;  // render task owns core 1
#else
  constexpr BaseType_t saverTaskCore = 0;
#endif
  if (xTaskCreatePinnedToCore(
          [](void* ctx) {
            auto* saver = static_cast<ProgressManager*>(ctx);
            for (;;) {
              vTaskDelay(pdMS_TO_TICKS(ProgressManager::FLUSH_INTERVAL_MS));
              if (saver->shouldFlush()) {
                saver->flushTick();
              }
            }
          },
          PROGRESS_TASK_NAME, SAVER_STACK_BYTES, this, PROGRESS_TASK_PRIORITY, nullptr, saverTaskCore) != pdPASS) {
    LOG_ERR("PRG", "Failed to create progress manager task");
  }
}

ProgressManager::~ProgressManager() {
  if (mutex_ != nullptr) {
    vSemaphoreDelete(mutex_);
  }
}

void ProgressManager::setBook(const char* cachePath) {
  if (mutex_ == nullptr) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  // Full reset, not just a pending-drop: a pending record or lastFlushed
  // from the PREVIOUS book must never influence the new book's change
  // detection (user-reported cross-book hazard, PR #107).
  state_.reset();
  if (cachePath == nullptr || cachePath[0] == '\0') {
    cachePath_[0] = '\0';
  } else {
    const int written = snprintf(cachePath_, sizeof(cachePath_), "%s", cachePath);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(cachePath_)) {
      // Truncated path would scatter progress.bin into a wrong directory;
      // disable flushing rather than write there (Copilot, PR #107).
      LOG_ERR("PRG", "Cache path too long for progress manager: %s", cachePath);
      cachePath_[0] = '\0';
      state_.reset();
    }
  }
  xSemaphoreGive(mutex_);
}

void ProgressManager::capture(const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                              const bool hasOffset, const uint32_t visibleTextOffset) {
  if (mutex_ == nullptr) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  state_.capture(makeRecord(spineIndex, pageNumber, pageCount, hasOffset, visibleTextOffset));
  xSemaphoreGive(mutex_);
}

bool ProgressManager::shouldFlush() const {
  if (mutex_ == nullptr) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool dirty = state_.shouldFlush();
  xSemaphoreGive(mutex_);
  return dirty;
}

void ProgressManager::seedLastFlushed(const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                                      const bool hasOffset, const uint32_t visibleTextOffset) {
  if (mutex_ == nullptr) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  state_.seedFlushed(makeRecord(spineIndex, pageNumber, pageCount, hasOffset, visibleTextOffset));
  xSemaphoreGive(mutex_);
}

void ProgressManager::publishPosition(const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                                      const bool hasOffset, const uint32_t visibleTextOffset) {
  if (mutex_ == nullptr) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  lastPublished_ = makeRecord(spineIndex, pageNumber, pageCount, hasOffset, visibleTextOffset);
  positionPublished_ = true;
  xSemaphoreGive(mutex_);
}

void ProgressManager::clearPosition() {
  if (mutex_ == nullptr) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  // Freshness checks pass when no reference is published (reader tearing
  // down / no book): the pending record will be dropped by the imminent
  // flushNow() + setBook(nullptr) instead.
  lastPublished_ = ProgressFlush::Record{};
  positionPublished_ = false;
  xSemaphoreGive(mutex_);
}

bool ProgressManager::saveNow(const char* cachePath, const uint16_t spineIndex, const uint16_t pageNumber,
                              const uint16_t pageCount, const bool hasOffset, const uint32_t visibleTextOffset) {
  if (mutex_ == nullptr) return false;
  // Capture-first (Copilot D6, PR #107): record the position through the
  // same change detection as the background path, THEN flush synchronously.
  // An unchanged record is a no-op (renderBook() runs for redraws too —
  // without this, low battery would rewrite an unchanged page on every
  // redraw), and a failed write leaves the record pending for the tick to
  // retry. The mutex is held for the whole capture+flush so no background
  // tick can interleave (single writer, design §4.7).
  xSemaphoreTake(mutex_, portMAX_DELAY);
  state_.capture(makeRecord(spineIndex, pageNumber, pageCount, hasOffset, visibleTextOffset));
  const bool ok = writePendingLocked();
  xSemaphoreGive(mutex_);
  return ok;
}

bool ProgressManager::writePending() {
  if (mutex_ == nullptr) return true;  // begin() never ran / OOM: fail closed
  // The mutex is held for the WHOLE flush (state take -> SD write -> state
  // update): flushNow() on the reader task and the periodic tick cannot run
  // two writeAtomic() calls against the same progress.bin.tmp concurrently,
  // and capture()/setBook() cannot interleave with an in-flight write, so
  // its completion can never re-arm or regress a superseded record
  // (CodeRabbit round-2, PR #107). The hold is bounded by one 10-byte
  // write (~ms).
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool ok = writePendingLocked();
  xSemaphoreGive(mutex_);
  return ok;
}

// Static record write: encode + atomic persist. The SINGLE place the
// progress.bin byte layout is produced (EpubReaderUtils::saveProgress and
// all internal flush paths route through here).
bool ProgressManager::saveRecord(const char* cachePath, const uint16_t spineIndex, const uint16_t pageNumber,
                                 const uint16_t pageCount, const bool hasOffset, const uint32_t visibleTextOffset) {
  uint8_t data[RECORD_SIZE_OFFSET];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  size_t dataSize = RECORD_SIZE_BASE;
  if (hasOffset) {
    data[6] = visibleTextOffset & 0xFF;
    data[7] = (visibleTextOffset >> 8) & 0xFF;
    data[8] = (visibleTextOffset >> 16) & 0xFF;
    data[9] = (visibleTextOffset >> 24) & 0xFF;
    dataSize = RECORD_SIZE_OFFSET;
  }
  if (!ProgressFile::writeAtomic(cachePath, data, dataSize)) {
    return false;
  }
  LOG_DBG("PRG", "Record written: spine=%u offset=%u page=%u", spineIndex, visibleTextOffset, pageNumber);
  return true;
}

size_t ProgressManager::load(const char* cachePath, uint16_t& spineIndex, uint16_t& pageNumber, uint16_t& pageCount,
                             uint32_t& visibleTextOffset) {
  HalFile f;
  if (!Storage.openFileForRead("PRG", std::string(cachePath) + "/progress.bin", f)) {
    return 0;
  }
  uint8_t data[RECORD_SIZE_OFFSET];
  const int n = f.read(data, sizeof(data));
  if (n != static_cast<int>(RECORD_SIZE_BASE) && n != static_cast<int>(RECORD_SIZE_OFFSET)) {
    return 0;  // missing / garbage / short record
  }
  spineIndex = static_cast<uint16_t>(data[0] + (data[1] << 8));
  pageNumber = static_cast<uint16_t>(data[2] + (data[3] << 8));
  pageCount = static_cast<uint16_t>(data[4] + (data[5] << 8));
  if (n == static_cast<int>(RECORD_SIZE_OFFSET)) {
    visibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                        (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
  } else {
    visibleTextOffset = 0;
  }
  return static_cast<size_t>(n);
}

bool ProgressManager::writePendingLocked() {
  // Mutex already held (saveNow() or writePending() caller).
  ProgressFlush::Record record;
  if (!state_.beginFlush(record)) {
    return true;  // nothing pending
  }
  if (cachePath_[0] == '\0' || (positionPublished_ && !(record == lastPublished_))) {
    // No book registered (KOReader path released the epub before teardown),
    // or the reader's published position has moved past the pending record
    // (captured before a re-pagination / newer render): drop the write —
    // treat as written so the dirty flag does not retry forever.
    LOG_DBG("PRG", "Flush dropped (no book=%d or stale: rec %u/%u vs pos %u/%u)", cachePath_[0] == '\0',
            record.spineIndex, record.pageNumber, lastPublished_.spineIndex, lastPublished_.pageNumber);
    state_.endFlush(record, true);
    return true;
  }
  const bool ok = saveRecord(cachePath_, record.spineIndex, record.pageNumber, record.pageCount, record.hasOffset,
                             record.visibleTextOffset);
  state_.endFlush(record, ok);
  if (ok) {
    LOG_INF("PRG", "Progress saved: spine=%u page=%u/%u", record.spineIndex, record.pageNumber, record.pageCount);
  } else {
    LOG_ERR("PRG", "Progress save FAILED: spine=%u page=%u/%u (will retry)", record.spineIndex, record.pageNumber);
  }
  return ok;
}

void ProgressManager::flushTick() {
  if (!shouldFlush()) {
    return;  // idle tick: nothing captured since the last flush
  }
  const bool ok = writePending();
  if (ok) {
    LOG_DBG("PRG", "Timer flush done");
  } else {
    LOG_ERR("PRG", "Background progress flush failed (will retry next tick)");
  }
}

bool ProgressManager::flushNow() {
  if (writePending()) {
    return true;
  }
  LOG_ERR("PRG", "Progress flush failed on exit");
  return false;
}