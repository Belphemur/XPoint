#include "ProgressManager.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "activities/reader/ProgressFile.h"

namespace {
// Low priority: progress persistence must never compete with rendering or
// input. Pinned to core 0 on dual-core boards (the render task owns core 1,
// ActivityManager.cpp); on single-core boards core 0 is the only core.
// NOTE: this ESP-IDF build passes usStackDepth in BYTES (the render task
// passes 8192 directly), not FreeRTOS words.
constexpr UBaseType_t WORKER_PRIORITY = 1;
constexpr size_t WORKER_STACK_BYTES = 2048;
constexpr char WORKER_TASK_NAME[] = "progress_mgr";
constexpr char MUTEX_TAG[] = "PRG";
}  // namespace

ProgressManager progressManager;

void ProgressManager::begin() {
  if (mutex_ != nullptr) return;  // idempotent
  mutex_ = xSemaphoreCreateMutex();
  if (mutex_ == nullptr) {
    LOG_ERR(MUTEX_TAG, "OOM: progress mutex");
    return;
  }
  // Dynamic allocation via the pool (user directive): PSRAM-backed on
  // BOARD_HAS_PSRAM boards, DRAM otherwise. Null on OOM = every operation
  // becomes a safe no-op (fail closed).
  auto cur = poolMakeBytes(sizeof(ProgressManager::Record));
  auto last = poolMakeBytes(sizeof(ProgressManager::Record));
  if (cur == nullptr || last == nullptr) {
    LOG_ERR(MUTEX_TAG, "OOM: progress state");
    return;  // pool bytes free themselves; mutex stays, ops no-op on null state
  }
  current_ = reinterpret_cast<ProgressManager::Record*>(cur.get());
  lastFlushed_ = reinterpret_cast<ProgressManager::Record*>(last.get());
  *current_ = ProgressManager::Record{};
  *lastFlushed_ = ProgressManager::Record{};
  // Ownership released to the raw members; the destructor poolFree()s them.
  (void)cur.release();
  (void)last.release();
  if (xTaskCreatePinnedToCore(
          [](void* ctx) {
            auto* self = static_cast<ProgressManager*>(ctx);
            for (;;) {
              // Block until a save() queues a write; cap the block at
              // FLUSH_INTERVAL_MS so a queued write also fires within one
              // interval even if its notification raced a flush.
              if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FLUSH_INTERVAL_MS)) == 0 && !self->writeQueued_) {
                continue;  // interval elapsed with nothing owed
              }
              xSemaphoreTake(self->mutex_, portMAX_DELAY);
              const bool ok = self->flushChangedLocked();
              xSemaphoreGive(self->mutex_);
              if (!ok) {
                LOG_ERR(MUTEX_TAG, "Worker flush failed (queued retry on next save)");
              }
            }
          },
          WORKER_TASK_NAME, WORKER_STACK_BYTES, this, WORKER_PRIORITY, &worker_, 0) != pdPASS) {
    LOG_ERR(MUTEX_TAG, "Failed to create progress worker task");
  }
}

ProgressManager::~ProgressManager() {
  if (current_ != nullptr) {
    poolFree(current_);
    current_ = nullptr;
  }
  if (lastFlushed_ != nullptr) {
    poolFree(lastFlushed_);
    lastFlushed_ = nullptr;
  }
  if (mutex_ != nullptr) {
    vSemaphoreDelete(mutex_);
  }
}

bool ProgressManager::openBook(const char* cachePath, uint16_t& spineIndex, uint16_t& pageNumber, uint16_t& pageCount,
                               uint32_t& visibleTextOffset) {
  if (mutex_ == nullptr || current_ == nullptr) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  // Full reset: a previous book's state must never seed this book's gate.
  *current_ = ProgressManager::Record{};
  *lastFlushed_ = ProgressManager::Record{};
  writeQueued_ = false;
  lastFlushMs_ = millis();

  if (cachePath == nullptr || cachePath[0] == '\0') {
    cachePath_[0] = '\0';
    bookOpen_ = false;
    xSemaphoreGive(mutex_);
    return false;
  }
  const int written = snprintf(cachePath_, sizeof(cachePath_), "%s", cachePath);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(cachePath_)) {
    // Truncated path would scatter progress.bin into a wrong directory;
    // disable rather than write there (Copilot, PR #107).
    LOG_ERR(MUTEX_TAG, "Cache path too long: %s", cachePath);
    cachePath_[0] = '\0';
    bookOpen_ = false;
    xSemaphoreGive(mutex_);
    return false;
  }
  bookOpen_ = true;

  const size_t size = load(cachePath_, spineIndex, pageNumber, pageCount, visibleTextOffset);
  if (size > 0) {
    const bool hasOffset = (size == RECORD_SIZE_OFFSET);
    current_->spineIndex = spineIndex;
    current_->pageNumber = pageNumber;
    current_->pageCount = pageCount;
    current_->hasOffset = hasOffset;
    current_->visibleTextOffset = visibleTextOffset;
    *lastFlushed_ = *current_;  // disk state IS the baseline
    LOG_INF(MUTEX_TAG, "Progress loaded: spine=%u page=%u/%u", spineIndex, pageNumber, pageCount);
  }
  xSemaphoreGive(mutex_);
  return size > 0;
}

void ProgressManager::save(const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                           const bool hasOffset, const uint32_t visibleTextOffset) {
  if (mutex_ == nullptr || current_ == nullptr || !bookOpen_) return;
  bool due = false;
  {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    current_->spineIndex = spineIndex;
    current_->pageNumber = pageNumber;
    current_->pageCount = pageCount;
    current_->hasOffset = hasOffset;
    current_->visibleTextOffset = visibleTextOffset;

    // Gate (design §4.2): write only when the position changed since the
    // last flush AND the interval elapsed — unless low battery, which
    // persists every change (§4.5). The battery is queried at gate time
    // (HAL caches it at BATTERY_POLL_MS); no flag feeding needed.
    const bool changed = !(*current_ == *lastFlushed_);
    const bool intervalElapsed = (millis() - lastFlushMs_) >= FLUSH_INTERVAL_MS;
    due = changed && (intervalElapsed || lowBattery() || writeQueued_);
    if (due) writeQueued_ = true;
  }
  if (due && worker_ != nullptr) {
    // Wake the worker now: it performs the write on core 0, off the reader
    // task. The notification is consumed together with writeQueued_.
    xTaskNotifyGive(worker_);
  }
}

bool ProgressManager::saveNow(const char* cachePath, const uint16_t spineIndex, const uint16_t pageNumber,
                              const uint16_t pageCount, const bool hasOffset, const uint32_t visibleTextOffset) {
  if (mutex_ == nullptr || current_ == nullptr) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool ok = saveRecord(cachePath, spineIndex, pageNumber, pageCount, hasOffset, visibleTextOffset);
  if (ok) {
    // Only update the in-memory baseline when THIS is the open book's file;
    // a bypass save for another path must not mark our book as flushed.
    if (bookOpen_ && strncmp(cachePath, cachePath_, sizeof(cachePath_)) == 0) {
      current_->spineIndex = spineIndex;
      current_->pageNumber = pageNumber;
      current_->pageCount = pageCount;
      current_->hasOffset = hasOffset;
      current_->visibleTextOffset = visibleTextOffset;
      *lastFlushed_ = *current_;
      lastFlushMs_ = millis();
      writeQueued_ = false;
    }
  }
  xSemaphoreGive(mutex_);
  return ok;
}

void ProgressManager::closeBook() {
  if (mutex_ == nullptr || current_ == nullptr) return;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  flushChangedLocked();
  *current_ = ProgressManager::Record{};
  *lastFlushed_ = ProgressManager::Record{};
  cachePath_[0] = '\0';
  bookOpen_ = false;
  writeQueued_ = false;
  xSemaphoreGive(mutex_);
  LOG_DBG(MUTEX_TAG, "Book closed, progress state reset");
}

bool ProgressManager::flushNow() {
  if (mutex_ == nullptr || current_ == nullptr) return true;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool ok = flushChangedLocked();
  xSemaphoreGive(mutex_);
  return ok;
}

bool ProgressManager::flushChangedLocked() {
  // Mutex held. Write current_ only when it differs from what is on disk.
  if (!bookOpen_ || cachePath_[0] == '\0' || *current_ == *lastFlushed_) {
    writeQueued_ = false;
    return true;  // nothing to do
  }
  const bool ok = saveRecord(cachePath_, current_->spineIndex, current_->pageNumber, current_->pageCount,
                             current_->hasOffset, current_->visibleTextOffset);
  if (ok) {
    *lastFlushed_ = *current_;
    lastFlushMs_ = millis();
    LOG_INF(MUTEX_TAG, "Progress saved: spine=%u page=%u/%u", current_->spineIndex, current_->pageNumber,
            current_->pageCount);
  } else {
    // Keep writeQueued_ set: the next save() (any page change) retries.
    LOG_ERR(MUTEX_TAG, "Progress save FAILED: spine=%u page=%u/%u (will retry)", current_->spineIndex,
            current_->pageNumber, current_->pageCount);
  }
  writeQueued_ = false;
  return ok;
}

// Gate semantics (design §4.5): below LOW_BATTERY_PERCENT, gauge HEALTHY,
// NOT charging — charging means external power, so there is nothing to
// protect. Pure HAL reads (the singleton's own cached values): static,
// no member state touched.
bool ProgressManager::lowBattery() {
  if (powerManager.isBatteryCharging()) return false;
  if (powerManager.getBatteryHealthState() != HalPowerManager::BatteryHealthState::HEALTHY) return false;
  return powerManager.getBatteryPercentage() < LOW_BATTERY_PERCENT;
}

size_t ProgressManager::load(const char* cachePath, uint16_t& spineIndex, uint16_t& pageNumber, uint16_t& pageCount,
                             uint32_t& visibleTextOffset) {
  HalFile f;
  if (!Storage.openFileForRead(MUTEX_TAG, std::string(cachePath) + "/progress.bin", f)) {
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
  LOG_DBG(MUTEX_TAG, "Record written: spine=%u offset=%u page=%u", spineIndex, visibleTextOffset, pageNumber);
  return true;
}