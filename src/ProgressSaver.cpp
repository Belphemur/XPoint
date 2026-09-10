#include "ProgressSaver.h"

#include <Epub.h>
#include <Logging.h>

#include <optional>

#include "activities/reader/EpubReaderUtils.h"

namespace {
// Low priority: progress persistence must never compete with rendering or
// input. Pinned to core 0 on dual-core boards (the render task owns core 1,
// ActivityManager.cpp) so the flusher cannot time-slice with a render; on
// single-core boards priority 1 keeps it behind the render task at the same
// level only when the render task blocks. Stack verified against
// uxTaskGetStackHighWaterMark() on device (design §5, review N2).
constexpr UBaseType_t SAVER_TASK_PRIORITY = 1;
constexpr size_t SAVER_STACK_WORDS = 2048 / sizeof(StackType_t);
constexpr char SAVER_TASK_NAME[] = "progress_saver";

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
}  // namespace

ProgressSaver progressSaver;

void ProgressSaver::begin() {
  if (mutex_ == nullptr) {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
      LOG_ERR("PRG", "OOM: progress saver mutex");
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
            auto* saver = static_cast<ProgressSaver*>(ctx);
            for (;;) {
              vTaskDelay(pdMS_TO_TICKS(ProgressSaver::FLUSH_INTERVAL_MS));
              if (saver->shouldFlush()) {
                saver->flushTick();
              }
            }
          },
          SAVER_TASK_NAME, SAVER_STACK_WORDS, this, SAVER_TASK_PRIORITY, nullptr, saverTaskCore) != pdPASS) {
    LOG_ERR("PRG", "Failed to create progress saver task");
  }
}

ProgressSaver::~ProgressSaver() {
  if (mutex_ != nullptr) {
    vSemaphoreDelete(mutex_);
  }
}

void ProgressSaver::setBook(const char* cachePath) {
  if (mutex_ == nullptr) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  // A pending record belongs to the PREVIOUS book: drop it, never write it
  // into the new book's cache dir.
  state_.clearPending();
  if (cachePath == nullptr || cachePath[0] == '\0') {
    cachePath_[0] = '\0';
  } else {
    const int written = snprintf(cachePath_, sizeof(cachePath_), "%s", cachePath);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(cachePath_)) {
      // Truncated path would scatter progress.bin into a wrong directory;
      // disable flushing rather than write there (Copilot, PR #107).
      LOG_ERR("PRG", "Cache path too long for progress saver: %s", cachePath);
      cachePath_[0] = '\0';
      state_.clearPending();
    }
  }
  xSemaphoreGive(mutex_);
}

void ProgressSaver::capture(const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                            const bool hasOffset, const uint32_t visibleTextOffset) {
  if (mutex_ == nullptr) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  state_.capture(makeRecord(spineIndex, pageNumber, pageCount, hasOffset, visibleTextOffset));
  xSemaphoreGive(mutex_);
}

bool ProgressSaver::shouldFlush() const {
  if (mutex_ == nullptr) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool dirty = state_.shouldFlush();
  xSemaphoreGive(mutex_);
  return dirty;
}

bool ProgressSaver::saveNow(const char* cachePath, const uint16_t spineIndex, const uint16_t pageNumber,
                            const uint16_t pageCount, const bool hasOffset, const uint32_t visibleTextOffset) {
  if (mutex_ == nullptr) return false;
  // Whole-flush mutex hold: no background tick can interleave, and this
  // record becomes lastFlushed atomically with the write itself (the saver
  // is the single writer of progress state, design §4.7).
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool ok = EpubReaderUtils::saveProgress(cachePath, spineIndex, pageNumber, pageCount,
                                                hasOffset ? std::optional<uint32_t>(visibleTextOffset) : std::nullopt);
  if (ok) {
    ProgressFlush::Record written = makeRecord(spineIndex, pageNumber, pageCount, hasOffset, visibleTextOffset);
    state_.markFlushed(written);
  }
  xSemaphoreGive(mutex_);
  return ok;
}

bool ProgressSaver::writePending() {
  if (mutex_ == nullptr) return true;  // begin() never ran / OOM: fail closed
  ProgressFlush::Record record;
  // The mutex is held for the WHOLE flush (state take -> SD write -> state
  // update): flushNow() on the reader task and the periodic tick cannot run
  // two writeAtomic() calls against the same progress.bin.tmp concurrently,
  // and capture()/setBook() cannot interleave with an in-flight write, so
  // its completion can never re-arm or regress a superseded record
  // (CodeRabbit round-2, PR #107). The hold is bounded by one 10-byte
  // write (~ms).
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (!state_.beginFlush(record)) {
    xSemaphoreGive(mutex_);
    return true;  // nothing pending
  }
  if (cachePath_[0] == '\0') {
    // No book registered (KOReader path released the epub before teardown):
    // treat as written so the dirty flag does not retry forever.
    state_.endFlush(record, true);
    xSemaphoreGive(mutex_);
    return true;
  }
  const bool ok = EpubReaderUtils::saveProgress(
      cachePath_, record.spineIndex, record.pageNumber, record.pageCount,
      record.hasOffset ? std::optional<uint32_t>(record.visibleTextOffset) : std::nullopt);
  state_.endFlush(record, ok);
  xSemaphoreGive(mutex_);
  return ok;
}

void ProgressSaver::flushTick() {
  if (!writePending()) {
    LOG_ERR("PRG", "Background progress flush failed (will retry next tick)");
  }
}

bool ProgressSaver::flushNow() {
  if (writePending()) {
    return true;
  }
  LOG_ERR("PRG", "Progress flush failed on exit");
  return false;
}