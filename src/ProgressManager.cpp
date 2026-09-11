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
// The flush path reaches deep into SdFat (write + flush + remove + rename
// through HalStorage), which needs well over 2 KB — 2048 B overflowed the
// stack canary on the first real flush (Guru Meditation on progress_mgr).
// 4096 keeps a comfortable margin; track it with the high-water-mark log in
// the worker loop.
constexpr size_t WORKER_STACK_BYTES = 4096;
constexpr char WORKER_TASK_NAME[] = "progress_mgr";
constexpr char MUTEX_TAG[] = "PRG";
}  // namespace

ProgressManager progressManager;

void ProgressManager::begin() {
  if (diskMutex_ != nullptr) return;  // idempotent
  diskMutex_ = xSemaphoreCreateMutex();
  if (diskMutex_ == nullptr) {
    LOG_ERR(MUTEX_TAG, "OOM: progress disk mutex");
    return;
  }
  LOG_DBG(MUTEX_TAG, "begin(): disk mutex created");
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
                LOG_DBG(MUTEX_TAG, "worker: %lus elapsed, nothing owed", FLUSH_INTERVAL_MS / 1000);
                continue;  // interval elapsed with nothing owed
              }
              LOG_INF(MUTEX_TAG, "worker: wake (queued=%d)", self->writeQueued_ ? 1 : 0);
              const bool ok = self->flushChanged();
              LOG_DBG(MUTEX_TAG, "worker: stack high-water=%u bytes",
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
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
  if (diskMutex_ != nullptr) {
    vSemaphoreDelete(diskMutex_);
  }
}

bool ProgressManager::openBook(const char* cachePath, uint16_t& spineIndex, uint16_t& pageNumber, uint16_t& pageCount,
                               uint32_t& visibleTextOffset) {
  if (diskMutex_ == nullptr || current_ == nullptr) {
    LOG_DBG(MUTEX_TAG, "openBook(): unavailable (disk mutex/state null)");
    return false;
  }
  LOG_DBG(MUTEX_TAG, "openBook(): path=%s", cachePath ? cachePath : "null");
  // Full reset: a previous book's state must never seed this book's gate.
  *current_ = ProgressManager::Record{};
  *lastFlushed_ = ProgressManager::Record{};
  writeQueued_ = false;
  lastFlushSec_ = static_cast<uint32_t>(millis() / 1000);
  LOG_DBG(MUTEX_TAG, "openBook(): prior state reset");

  if (cachePath == nullptr || cachePath[0] == '\0') {
    cachePath_[0] = '\0';
    bookOpen_ = false;
    LOG_DBG(MUTEX_TAG, "openBook(): empty path, disabled");
    return false;
  }
  const int written = snprintf(cachePath_, sizeof(cachePath_), "%s", cachePath);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(cachePath_)) {
    // Truncated path would scatter progress.bin into a wrong directory;
    // disable rather than write there (Copilot, PR #107).
    LOG_ERR(MUTEX_TAG, "Cache path too long: %s", cachePath);
    LOG_DBG(MUTEX_TAG, "openBook(): aborted (path too long)");
    cachePath_[0] = '\0';
    bookOpen_ = false;
    return false;
  }
  bookOpen_ = true;

  // Disk read under the same lock as the writes: a flush's rename must
  // never interleave with this read of progress.bin.
  xSemaphoreTake(diskMutex_, portMAX_DELAY);
  const size_t size = load(cachePath_, spineIndex, pageNumber, pageCount, visibleTextOffset);
  xSemaphoreGive(diskMutex_);
  if (size > 0) {
    const bool hasOffset = (size == RECORD_SIZE_OFFSET);
    current_->spineIndex = spineIndex;
    current_->pageNumber = pageNumber;
    current_->pageCount = pageCount;
    current_->hasOffset = hasOffset;
    current_->visibleTextOffset = visibleTextOffset;
    *lastFlushed_ = *current_;  // disk state IS the baseline
    LOG_INF(MUTEX_TAG, "Progress loaded: spine=%u page=%u/%u", spineIndex, pageNumber, pageCount);
  } else {
    LOG_DBG(MUTEX_TAG, "openBook(): no existing progress record (size=%u)", size);
  }
  return size > 0;
}

void ProgressManager::save(const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                           const bool hasOffset, const uint32_t visibleTextOffset) {
  if (diskMutex_ == nullptr || current_ == nullptr || !bookOpen_) {
    LOG_DBG(MUTEX_TAG, "save(): DROPPED (diskMutex=%d state=%d bookOpen=%d)", diskMutex_ != nullptr,
            current_ != nullptr, bookOpen_ ? 1 : 0);
    return;
  }
  // No lock here: this runs on the render task in the page-turn path and
  // must never block on SD/battery I/O. The battery read (HAL I2C) happens
  // before touching any shared state so a stall can't corrupt the record.
  const bool lowBat = lowBattery();
  bool due = false;
  {
    // Lock-free state update: native-width field writes are atomic on this
    // core; the worker may read them mid-update and only ever persists a
    // slightly stale, self-correcting record.
    current_->spineIndex = spineIndex;
    current_->pageNumber = pageNumber;
    current_->pageCount = pageCount;
    current_->hasOffset = hasOffset;
    current_->visibleTextOffset = visibleTextOffset;

    // Gate (design §4.2): write only when the position changed since the
    // last flush AND the interval elapsed — unless low battery or a write
    // is already owed (§4.5).
    const bool changed = !(*current_ == *lastFlushed_);
    const uint32_t sinceFlushSec = static_cast<uint32_t>(millis() / 1000) - lastFlushSec_;
    const bool intervalElapsed = sinceFlushSec >= (FLUSH_INTERVAL_MS / 1000);
    due = changed && (intervalElapsed || lowBat || writeQueued_);
    if (due) writeQueued_ = true;
    LOG_INF(MUTEX_TAG,
            "save(): spine=%u page=%u/%u offset=%u changed=%d sinceFlush=%lus/%lus lowbat=%d queued=%d "
            "due=%d",
            spineIndex, pageNumber, pageCount, visibleTextOffset, changed ? 1 : 0,
            static_cast<unsigned long>(sinceFlushSec), static_cast<unsigned long>(FLUSH_INTERVAL_MS / 1000),
            lowBat ? 1 : 0, writeQueued_ ? 1 : 0, due ? 1 : 0);
  }
  if (due && worker_ != nullptr) {
    // Wake the worker now: it performs the write on core 0, off the reader
    // task. The notification is consumed together with writeQueued_.
    xTaskNotifyGive(worker_);
  }
}

bool ProgressManager::saveNow(const char* cachePath, const uint16_t spineIndex, const uint16_t pageNumber,
                              const uint16_t pageCount, const bool hasOffset, const uint32_t visibleTextOffset) {
  if (diskMutex_ == nullptr || current_ == nullptr) {
    LOG_DBG(MUTEX_TAG, "saveNow(): unavailable (disk mutex/state null)");
    return false;
  }
  LOG_DBG(MUTEX_TAG, "saveNow(): spine=%u page=%u/%u offset=%u", spineIndex, pageNumber, pageCount, visibleTextOffset);
  const bool ok = saveRecordLocked(cachePath, spineIndex, pageNumber, pageCount, hasOffset, visibleTextOffset);
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
      lastFlushSec_ = static_cast<uint32_t>(millis() / 1000);
      writeQueued_ = false;
    }
  } else {
    LOG_DBG(MUTEX_TAG, "saveNow(): write failed");
  }
  return ok;
}

void ProgressManager::closeBook() {
  if (diskMutex_ == nullptr || current_ == nullptr) {
    LOG_DBG(MUTEX_TAG, "closeBook(): unavailable (disk mutex/state null)");
    return;
  }
  LOG_DBG(MUTEX_TAG, "closeBook(): flushing pending progress");
  flushChanged();
  *current_ = ProgressManager::Record{};
  *lastFlushed_ = ProgressManager::Record{};
  cachePath_[0] = '\0';
  bookOpen_ = false;
  writeQueued_ = false;
  LOG_DBG(MUTEX_TAG, "Book closed, progress state reset");
}

bool ProgressManager::flushNow() {
  if (diskMutex_ == nullptr || current_ == nullptr) {
    LOG_DBG(MUTEX_TAG, "flushNow(): unavailable (disk mutex/state null)");
    return true;
  }
  LOG_DBG(MUTEX_TAG, "flushNow(): forcing flush");
  return flushChanged();
}

bool ProgressManager::flushChanged() {
  // No lock: write current_ only when it differs from what is on disk. The
  // record fields are native-width reads; a concurrent save() may tear the
  // snapshot, which only ever yields a stale, self-correcting record.
  if (!bookOpen_ || cachePath_[0] == '\0' || *current_ == *lastFlushed_) {
    writeQueued_ = false;
    LOG_DBG(MUTEX_TAG, "flushChanged(): nothing to flush");
    return true;  // nothing to do
  }
  LOG_INF(MUTEX_TAG, "flushChanged(): flushing spine=%u page=%u/%u sinceFlush=%lus", current_->spineIndex,
          current_->pageNumber, current_->pageCount,
          static_cast<unsigned long>(static_cast<uint32_t>(millis() / 1000) - lastFlushSec_));
  const bool ok = saveRecordLocked(cachePath_, current_->spineIndex, current_->pageNumber, current_->pageCount,
                                   current_->hasOffset, current_->visibleTextOffset);
  if (ok) {
    *lastFlushed_ = *current_;
    lastFlushSec_ = static_cast<uint32_t>(millis() / 1000);
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

bool ProgressManager::saveRecordLocked(const char* cachePath, const uint16_t spineIndex, const uint16_t pageNumber,
                                       const uint16_t pageCount, const bool hasOffset,
                                       const uint32_t visibleTextOffset) {
  xSemaphoreTake(diskMutex_, portMAX_DELAY);
  const bool ok = saveRecord(cachePath, spineIndex, pageNumber, pageCount, hasOffset, visibleTextOffset);
  xSemaphoreGive(diskMutex_);
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
    LOG_DBG(MUTEX_TAG, "load(): no progress.bin at %s", cachePath);
    return 0;
  }
  uint8_t data[RECORD_SIZE_OFFSET];
  const int n = f.read(data, sizeof(data));
  if (n != static_cast<int>(RECORD_SIZE_BASE) && n != static_cast<int>(RECORD_SIZE_OFFSET)) {
    LOG_DBG(MUTEX_TAG, "load(): malformed record (read=%d)", n);
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