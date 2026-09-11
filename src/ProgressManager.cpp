#include "ProgressManager.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "activities/reader/ProgressFile.h"
#include "activities/reader/ProgressRecord.h"

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
    // A generation-tagged (16-byte) TTF record read through the legacy open
    // degrades: its charOffset slot is NOT a visible text offset.
    const bool hasOffset = (size == progress_record::kSizeOffset);
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

#if defined(CROSSPOINT_TTF_READER)
bool ProgressManager::openBookTtf(const char* cachePath, uint16_t& spineIndex, uint16_t& pageNumber,
                                  uint16_t& pageCount, uint32_t& charOffset, uint32_t& generation,
                                  bool& hasGeneration) {
  charOffset = 0;
  generation = 0;
  hasGeneration = false;
  // Base restore first: spine/page/pageCount + legacy-shape degrade.
  uint32_t baseOffset = 0;
  if (!openBook(cachePath, spineIndex, pageNumber, pageCount, baseOffset)) return false;

  // Inspect the on-disk layout for the generation-tagged shape (same mutex
  // discipline as openBook's read).
  xSemaphoreTake(diskMutex_, portMAX_DELAY);
  HalFile f;
  bool gen = false;
  if (Storage.openFileForRead(MUTEX_TAG, std::string(cachePath) + "/progress.bin", f)) {
    const uint64_t fileSize = f.fileSize64();
    uint8_t data[progress_record::kSizeGeneration + 1];
    const size_t bytesToRead =
        fileSize > progress_record::kSizeGeneration ? progress_record::kSizeGeneration + 1
                                                     : static_cast<size_t>(fileSize);
    const int n = f.read(data, bytesToRead);
    ProgressRecord rec;
    if (n == static_cast<int>(bytesToRead) &&
        progress_record::decode(data, static_cast<size_t>(n), rec) == progress_record::kSizeGeneration) {
      charOffset = rec.charOffset;
      generation = rec.generation;
      hasGeneration = true;
      gen = true;
    }
  }
  xSemaphoreGive(diskMutex_);
  if (gen) {
    current_->hasOffset = false;
    current_->hasGeneration = true;
    current_->generation = generation;
    current_->visibleTextOffset = charOffset;  // char-offset carrier
    *lastFlushed_ = *current_;
    LOG_INF(MUTEX_TAG, "TTF progress loaded: spine=%u page=%u charOffset=%u gen=%u", spineIndex, pageNumber, charOffset,
            generation);
  } else {
    LOG_DBG(MUTEX_TAG, "openBookTtf(): legacy record — chapter-start degrade");
  }
  return true;
}

void ProgressManager::saveTtf(const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                              const uint32_t charOffset, const uint32_t generation) {
  if (diskMutex_ == nullptr || current_ == nullptr || !bookOpen_) {
    LOG_DBG(MUTEX_TAG, "saveTtf(): DROPPED (state/bookOpen)");
    return;
  }
  const bool lowBat = lowBattery();
  bool due = false;
  {
    current_->spineIndex = spineIndex;
    current_->pageNumber = pageNumber;
    current_->pageCount = pageCount;
    current_->hasOffset = false;
    current_->hasGeneration = true;
    current_->generation = generation;
    current_->visibleTextOffset = charOffset;  // char-offset carrier

    const bool changed = !(*current_ == *lastFlushed_);
    const uint32_t sinceFlushSec = static_cast<uint32_t>(millis() / 1000) - lastFlushSec_;
    const bool intervalElapsed = sinceFlushSec >= (FLUSH_INTERVAL_MS / 1000);
    due = changed && (intervalElapsed || lowBat || writeQueued_);
    if (due) writeQueued_ = true;
    LOG_INF(MUTEX_TAG, "saveTtf(): spine=%u page=%u/%u charOffset=%u gen=%u due=%d", spineIndex, pageNumber, pageCount,
            charOffset, generation, due ? 1 : 0);
  }
  if (due && worker_ != nullptr) {
    xTaskNotifyGive(worker_);
  }
}
#endif

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
#if defined(CROSSPOINT_TTF_READER)
    // A legacy-shape save (Txt/Xtc readers, bitmap reader) migrates the
    // record back down; the next TTF save rewrites the 16-byte shape.
    current_->hasGeneration = false;
    current_->generation = 0;
#endif

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
  Record rec;
  rec.spineIndex = spineIndex;
  rec.pageNumber = pageNumber;
  rec.pageCount = pageCount;
  rec.hasOffset = hasOffset;
  rec.visibleTextOffset = visibleTextOffset;
  const bool ok = saveNowRecord(cachePath, rec);
  return ok;
}

#if defined(CROSSPOINT_TTF_READER)
bool ProgressManager::saveNowTtf(const char* cachePath, const uint16_t spineIndex, const uint16_t pageNumber,
                                 const uint16_t pageCount, const uint32_t charOffset, const uint32_t generation) {
  if (diskMutex_ == nullptr || current_ == nullptr) {
    LOG_DBG(MUTEX_TAG, "saveNowTtf(): unavailable (disk mutex/state null)");
    return false;
  }
  LOG_DBG(MUTEX_TAG, "saveNowTtf(): spine=%u page=%u/%u charOffset=%u gen=%u", spineIndex, pageNumber, pageCount,
          charOffset, generation);
  Record rec;
  rec.spineIndex = spineIndex;
  rec.pageNumber = pageNumber;
  rec.pageCount = pageCount;
  rec.hasOffset = false;
  rec.hasGeneration = true;
  rec.generation = generation;
  rec.visibleTextOffset = charOffset;
  return saveNowRecord(cachePath, rec);
}
#endif

// Shared body of the synchronous bypass saves (legacy + TTF record shapes).
bool ProgressManager::saveNowRecord(const char* cachePath, const Record& rec) {
  if (diskMutex_ == nullptr || current_ == nullptr) {
    LOG_DBG(MUTEX_TAG, "saveNow(): unavailable (disk mutex/state null)");
    return false;
  }
  const bool ok = saveRecordLocked(cachePath, rec);
  if (ok) {
    // Only update the in-memory baseline when THIS is the open book's file;
    // a bypass save for another path must not mark our book as flushed.
    if (bookOpen_ && strncmp(cachePath, cachePath_, sizeof(cachePath_)) == 0) {
      *current_ = rec;
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
  const bool ok = saveRecordLocked(cachePath_, *current_);
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

bool ProgressManager::saveRecordLocked(const char* cachePath, const Record& rec) {
  xSemaphoreTake(diskMutex_, portMAX_DELAY);
  const bool ok = saveRecord(cachePath, rec);
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
  const uint64_t fileSize = f.fileSize64();
  if (fileSize < progress_record::kSizeBase) {
    LOG_DBG(MUTEX_TAG, "load(): progress record too short (%llu bytes)", static_cast<unsigned long long>(fileSize));
    return 0;
  }
  uint8_t data[progress_record::kSizeGeneration + 1];
  const size_t bytesToRead =
      fileSize > progress_record::kSizeGeneration ? progress_record::kSizeGeneration + 1
                                                   : static_cast<size_t>(fileSize);
  const int n = f.read(data, bytesToRead);
  ProgressRecord rec;
  const size_t size = n == static_cast<int>(bytesToRead)
                          ? progress_record::decode(data, static_cast<size_t>(n), rec)
                          : 0;
  if (size == 0) {
    LOG_DBG(MUTEX_TAG, "load(): malformed record (read=%d)", n);
    return 0;  // missing / garbage / short record
  }
  // A 16-byte TTF record read through the legacy path keeps only the base
  // triple (hasOffset false): charOffset is NOT a visible text offset.
  spineIndex = rec.spineIndex;
  pageNumber = rec.pageNumber;
  pageCount = rec.pageCount;
  visibleTextOffset = rec.visibleTextOffset;
  return size;
}

bool ProgressManager::saveRecord(const char* cachePath, const Record& rec) {
  uint8_t data[progress_record::kSizeGeneration];
  const size_t dataSize = progress_record::encode(rec.hasOffset,
#if defined(CROSSPOINT_TTF_READER)
                                                  rec.hasGeneration,
#else
                                                  false,
#endif
                                                  rec.spineIndex, rec.pageNumber, rec.pageCount, rec.visibleTextOffset,
                                                  rec.visibleTextOffset,
#if defined(CROSSPOINT_TTF_READER)
                                                  rec.generation,
#else
                                                  0,
#endif
                                                  data, sizeof(data));
  if (dataSize == 0) {
    return false;
  }
  if (!ProgressFile::writeAtomic(cachePath, data, dataSize)) {
    return false;
  }
  LOG_DBG(MUTEX_TAG, "Record written: spine=%u offset=%u page=%u", rec.spineIndex, rec.visibleTextOffset,
          rec.pageNumber);
  return true;
}