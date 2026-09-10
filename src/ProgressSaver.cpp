#include "ProgressSaver.h"

#include <Epub.h>
#include <Logging.h>

#include <optional>

#include "activities/reader/EpubReaderUtils.h"

namespace {
// Low priority: progress persistence must never compete with rendering or
// input. Stack verified against uxTaskGetStackHighWaterMark() on device
// (design §5, review N2).
constexpr UBaseType_t SAVER_TASK_PRIORITY = 1;
constexpr size_t SAVER_STACK_WORDS = 2048 / sizeof(StackType_t);
constexpr char SAVER_TASK_NAME[] = "progress_saver";
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
  if (xTaskCreate(
          [](void* ctx) {
            auto* saver = static_cast<ProgressSaver*>(ctx);
            for (;;) {
              vTaskDelay(pdMS_TO_TICKS(ProgressSaver::FLUSH_INTERVAL_MS));
              if (saver->shouldFlush()) {
                saver->flushTick();
              }
            }
          },
          SAVER_TASK_NAME, SAVER_STACK_WORDS, this, SAVER_TASK_PRIORITY, nullptr) != pdPASS) {
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
    snprintf(cachePath_, sizeof(cachePath_), "%s", cachePath);
  }
  xSemaphoreGive(mutex_);
}

void ProgressSaver::capture(const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                            const bool hasOffset, const uint32_t visibleTextOffset) {
  if (mutex_ == nullptr) return;
  ProgressFlush::Record record;
  record.spineIndex = spineIndex;
  record.pageNumber = pageNumber;
  record.pageCount = pageCount;
  record.hasOffset = hasOffset;
  record.visibleTextOffset = visibleTextOffset;

  xSemaphoreTake(mutex_, portMAX_DELAY);
  state_.capture(record);
  xSemaphoreGive(mutex_);
}

bool ProgressSaver::shouldFlush() const {
  if (mutex_ == nullptr) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool dirty = state_.shouldFlush();
  xSemaphoreGive(mutex_);
  return dirty;
}

void ProgressSaver::markFlushed(const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                                const bool hasOffset, const uint32_t visibleTextOffset) {
  if (mutex_ == nullptr) return;
  ProgressFlush::Record record;
  record.spineIndex = spineIndex;
  record.pageNumber = pageNumber;
  record.pageCount = pageCount;
  record.hasOffset = hasOffset;
  record.visibleTextOffset = visibleTextOffset;

  xSemaphoreTake(mutex_, portMAX_DELAY);
  state_.markFlushed(record);
  xSemaphoreGive(mutex_);
}

bool ProgressSaver::writePending() {
  ProgressFlush::Record record;
  {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!state_.beginFlush(record)) {
      xSemaphoreGive(mutex_);
      return true;  // nothing pending
    }
    xSemaphoreGive(mutex_);
  }
  if (cachePath_[0] == '\0') {
    // No book registered (KOReader path released the epub before teardown):
    // treat as written so the dirty flag does not retry forever.
    xSemaphoreTake(mutex_, portMAX_DELAY);
    state_.endFlush(record, true);
    xSemaphoreGive(mutex_);
    return true;
  }
  const bool ok = EpubReaderUtils::saveProgress(
      cachePath_, record.spineIndex, record.pageNumber, record.pageCount,
      record.hasOffset ? std::optional<uint32_t>(record.visibleTextOffset) : std::nullopt);
  xSemaphoreTake(mutex_, portMAX_DELAY);
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
