#include "ProgressManager.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "activities/reader/ProgressFile.h"
#include "activities/reader/ProgressRecord.h"

namespace {

// Unsigned-safe seconds since the last flush: millis()/1000 wraps (~49.7
// days); a wrapped 'now' must read as "interval not elapsed", not a huge
// number that would force a spurious flush.
uint32_t secsSinceFlush(const uint32_t lastSec) {
  const uint32_t now = static_cast<uint32_t>(millis() / 1000);
  return now >= lastSec ? now - lastSec : 0;
}
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
  if (stateMutex_ != nullptr || diskMutex_ != nullptr) return;  // idempotent
  stateMutex_ = xSemaphoreCreateMutex();
  if (stateMutex_ == nullptr) {
    LOG_ERR(MUTEX_TAG, "OOM: progress state mutex");
    return;
  }
  diskMutex_ = xSemaphoreCreateMutex();
  if (diskMutex_ == nullptr) {
    LOG_ERR(MUTEX_TAG, "OOM: progress disk mutex");
    vSemaphoreDelete(stateMutex_);
    stateMutex_ = nullptr;
    return;
  }
  LOG_DBG(MUTEX_TAG, "begin(): state and disk mutexes created");
  // Dynamic allocation via the pool (user directive): PSRAM-backed on
  // BOARD_HAS_PSRAM boards, DRAM otherwise. Null on OOM = every operation
  // becomes a safe no-op (fail closed).
  auto cur = poolMakeBytes(sizeof(ProgressManager::Record));
  auto last = poolMakeBytes(sizeof(ProgressManager::Record));
  if (cur == nullptr || last == nullptr) {
    LOG_ERR(MUTEX_TAG, "OOM: progress state");
    vSemaphoreDelete(diskMutex_);
    vSemaphoreDelete(stateMutex_);
    diskMutex_ = nullptr;
    stateMutex_ = nullptr;
    return;  // mutexes are gone; ops no-op on null state
  }
  current_ = reinterpret_cast<ProgressManager::Record*>(cur.get());
  lastFlushed_ = reinterpret_cast<ProgressManager::Record*>(last.get());
  *current_ = ProgressManager::Record{};
  *lastFlushed_ = ProgressManager::Record{};
  // Ownership released to the raw members; the destructor poolFree()s them.
  (void)cur.release();
  (void)last.release();
  // Exit handshake; see workerStopping_/workerExit_. Without the handshake
  // the worker must not run: a later destructor could otherwise free state
  // it still reads.
  workerExit_ = xSemaphoreCreateBinary();
  if (workerExit_ == nullptr) {
    LOG_ERR(MUTEX_TAG, "OOM: progress worker exit semaphore");
    return;
  }
  if (xTaskCreatePinnedToCore(
          [](void* ctx) {
            auto* self = static_cast<ProgressManager*>(ctx);
            for (;;) {
              // Block until a save() queues a write; cap the block at
              // FLUSH_INTERVAL_MS so a queued write also fires within one
              // interval even if its notification raced a flush.
              bool owed = false;
              if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FLUSH_INTERVAL_MS)) == 0) {
                if (self->workerStopping_) break;
                xSemaphoreTake(self->stateMutex_, portMAX_DELAY);
                owed = self->writeQueued_;
                xSemaphoreGive(self->stateMutex_);
                if (!owed) continue;  // interval elapsed with nothing owed
              }
              if (self->workerStopping_) break;
              LOG_INF(MUTEX_TAG, "worker: wake (queued=%d)", owed ? 1 : 0);
              const bool ok = self->flushChanged();
              LOG_DBG(MUTEX_TAG, "worker: stack high-water=%u bytes",
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
              if (!ok) {
                LOG_ERR(MUTEX_TAG, "Worker flush failed (queued retry on next save)");
              }
            }
            xSemaphoreGive(self->workerExit_);
            vTaskDelete(nullptr);
          },
          WORKER_TASK_NAME, WORKER_STACK_BYTES, this, WORKER_PRIORITY, &worker_, 0) != pdPASS) {
    LOG_ERR(MUTEX_TAG, "Failed to create progress worker task");
  }
}

ProgressManager::~ProgressManager() {
  // Quiesce the worker BEFORE releasing the state it reads. It acknowledges
  // exactly once: after its loop leaves either a blocked wake (notification
  // below) or an in-flight flush, and before vTaskDelete().
  if (worker_ != nullptr) {
    workerStopping_ = true;
    xTaskNotifyGive(worker_);
    if (workerExit_ != nullptr) {
      // A flush can take several seconds on slow SD; waiting here is safe in
      // the only teardown path (global static destruction after loop()).
      xSemaphoreTake(workerExit_, portMAX_DELAY);
    }
    worker_ = nullptr;
  }
  if (current_ != nullptr) {
    poolFree(current_);
    current_ = nullptr;
  }
  if (lastFlushed_ != nullptr) {
    poolFree(lastFlushed_);
    lastFlushed_ = nullptr;
  }
  if (stateMutex_ != nullptr) {
    vSemaphoreDelete(stateMutex_);
    stateMutex_ = nullptr;
  }
  if (diskMutex_ != nullptr) {
    vSemaphoreDelete(diskMutex_);
    diskMutex_ = nullptr;
  }
  if (workerExit_ != nullptr) {
    vSemaphoreDelete(workerExit_);
    workerExit_ = nullptr;
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
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  *current_ = ProgressManager::Record{};
  *lastFlushed_ = ProgressManager::Record{};
  writeQueued_ = false;
  lastFlushSec_ = static_cast<uint32_t>(millis() / 1000);
  xSemaphoreGive(stateMutex_);
  LOG_DBG(MUTEX_TAG, "openBook(): prior state reset");

  if (cachePath == nullptr || cachePath[0] == '\0') {
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    cachePath_[0] = '\0';
    bookOpen_ = false;
    xSemaphoreGive(stateMutex_);
    LOG_DBG(MUTEX_TAG, "openBook(): empty path, disabled");
    return false;
  }
  char localPath[sizeof(cachePath_)];
  const int written = snprintf(localPath, sizeof(localPath), "%s", cachePath);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(localPath)) {
    // Truncated path would scatter progress.bin into a wrong directory;
    // disable rather than write there (Copilot, PR #107).
    LOG_ERR(MUTEX_TAG, "Cache path too long: %s", cachePath);
    LOG_DBG(MUTEX_TAG, "openBook(): aborted (path too long)");
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    cachePath_[0] = '\0';
    bookOpen_ = false;
    xSemaphoreGive(stateMutex_);
    return false;
  }
  // Book activation + the baseline disk read hold diskMutex_ so a worker's
  // commitRecord — which validates the session under the same lock before
  // its disk write — can never land a stale record into this open (or be
  // read by this load). The generation bump invalidates any snapshot the
  // previous session's worker still carries.
  xSemaphoreTake(diskMutex_, portMAX_DELAY);
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  memcpy(cachePath_, localPath, sizeof(localPath));
  bookOpen_ = true;
  ++bookGeneration_;
  xSemaphoreGive(stateMutex_);

  const size_t size = load(cachePath_, spineIndex, pageNumber, pageCount, visibleTextOffset);
  xSemaphoreGive(diskMutex_);
  if (size > 0) {
    // A generation-tagged (16-byte) TTF record read through the legacy open
    // degrades: its charOffset slot is NOT a visible text offset.
    const bool hasOffset = (size == progress_record::kSizeOffset);
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    current_->spineIndex = spineIndex;
    current_->pageNumber = pageNumber;
    current_->pageCount = pageCount;
    current_->hasOffset = hasOffset;
    current_->visibleTextOffset = visibleTextOffset;
    *lastFlushed_ = *current_;  // disk state IS the baseline
    xSemaphoreGive(stateMutex_);
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
    const size_t bytesToRead = fileSize > progress_record::kSizeGeneration ? progress_record::kSizeGeneration + 1
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
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    current_->hasOffset = false;
    current_->hasGeneration = true;
    current_->generation = generation;
    current_->visibleTextOffset = charOffset;  // char-offset carrier
    *lastFlushed_ = *current_;
    xSemaphoreGive(stateMutex_);
    LOG_INF(MUTEX_TAG, "TTF progress loaded: spine=%u page=%u charOffset=%u gen=%u", spineIndex, pageNumber, charOffset,
            generation);
  } else {
    LOG_DBG(MUTEX_TAG, "openBookTtf(): legacy record — chapter-start degrade");
  }
  return true;
}

void ProgressManager::saveTtf(const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                              const uint32_t charOffset, const uint32_t generation) {
  if (diskMutex_ == nullptr || current_ == nullptr) {
    LOG_DBG(MUTEX_TAG, "saveTtf(): DROPPED (state mutex/state null)");
    return;
  }
  const bool lowBat = lowBattery();
  bool due = false;
  bool available = false;
  bool changed = false;
  uint32_t sinceFlushSec = 0;
  bool queued = false;
  {
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    available = bookOpen_;
    if (available) {
      current_->spineIndex = spineIndex;
      current_->pageNumber = pageNumber;
      current_->pageCount = pageCount;
      current_->hasOffset = false;
      current_->hasGeneration = true;
      current_->generation = generation;
      current_->visibleTextOffset = charOffset;  // char-offset carrier

      changed = !(*current_ == *lastFlushed_);
      sinceFlushSec = static_cast<uint32_t>(millis() / 1000) - lastFlushSec_;
      const bool intervalElapsed = sinceFlushSec >= (FLUSH_INTERVAL_MS / 1000);
      due = changed && (intervalElapsed || lowBat || writeQueued_);
      if (due) writeQueued_ = true;
      queued = writeQueued_;
    }
    xSemaphoreGive(stateMutex_);
  }
  if (!available) {
    LOG_DBG(MUTEX_TAG, "saveTtf(): DROPPED (book closed)");
    return;
  }
  LOG_INF(MUTEX_TAG,
          "saveTtf(): spine=%u page=%u/%u charOffset=%u gen=%u changed=%d sinceFlush=%lus lowbat=%d queued=%d due=%d",
          spineIndex, pageNumber, pageCount, charOffset, generation, changed ? 1 : 0,
          static_cast<unsigned long>(sinceFlushSec), lowBat ? 1 : 0, queued ? 1 : 0, due ? 1 : 0);
  if (due && worker_ != nullptr) {
    xTaskNotifyGive(worker_);
  }
}
#endif

void ProgressManager::save(const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                           const bool hasOffset, const uint32_t visibleTextOffset) {
  if (diskMutex_ == nullptr || current_ == nullptr) {
    LOG_DBG(MUTEX_TAG, "save(): DROPPED (disk mutex/state null)");
    return;
  }
  // Battery and SD I/O stay outside stateMutex_. Only the in-memory record
  // update and flush-gate comparison are protected.
  const bool lowBat = lowBattery();
  bool due = false;
  bool available = false;
  bool changed = false;
  uint32_t sinceFlushSec = 0;
  bool queued = false;
  {
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    available = bookOpen_;
    if (available) {
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
      changed = !(*current_ == *lastFlushed_);
      sinceFlushSec = secsSinceFlush(lastFlushSec_);
      const bool intervalElapsed = sinceFlushSec >= (FLUSH_INTERVAL_MS / 1000);
      due = changed && (intervalElapsed || lowBat || writeQueued_);
      if (due) writeQueued_ = true;
      queued = writeQueued_;
    }
    xSemaphoreGive(stateMutex_);
  }
  if (!available) {
    LOG_DBG(MUTEX_TAG, "save(): DROPPED (book closed)");
    return;
  }
  LOG_INF(MUTEX_TAG, "save(): spine=%u page=%u/%u offset=%u changed=%d sinceFlush=%lus lowbat=%d queued=%d due=%d",
          spineIndex, pageNumber, pageCount, visibleTextOffset, changed ? 1 : 0,
          static_cast<unsigned long>(sinceFlushSec), lowBat ? 1 : 0, queued ? 1 : 0, due ? 1 : 0);
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
  // Bypass saves (KOReader sync, cache-clear backup, footnote origin) are
  // authoritative: commitRecord adopts the record as the live mirror when it
  // differs from current_, so a worker or closeBook() flush cannot overwrite
  // the just-saved record with a different snapshot (Copilot, PR #112).
  return commitRecord(cachePath, rec, /*adopt=*/true);
}

void ProgressManager::closeBook() {
  if (diskMutex_ == nullptr || current_ == nullptr) {
    LOG_DBG(MUTEX_TAG, "closeBook(): unavailable (disk mutex/state null)");
    return;
  }
  LOG_DBG(MUTEX_TAG, "closeBook(): flushing pending progress");
  const bool flushed = flushChanged();
  // Session state changes hold diskMutex_ too: a worker mid-commitRecord
  // (which validates the session under the same lock before its disk write)
  // can never land a stale record after this reset.
  xSemaphoreTake(diskMutex_, portMAX_DELAY);
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  if (flushed) {
    *current_ = ProgressManager::Record{};
    *lastFlushed_ = ProgressManager::Record{};
    cachePath_[0] = '\0';
    bookOpen_ = false;
    writeQueued_ = false;
  } else {
    // The flush reported an uncommitted record (CodeRabbit, PR #112):
    // preserve the owed state so a later flushNow() (enterPowerOff) can
    // retry the write if the SD error was transient. The next openBook()
    // resets this state either way; clearing it here would silently lose
    // the newest position.
    LOG_ERR(MUTEX_TAG, "closeBook(): flush failed — progress state preserved for retry");
  }
  xSemaphoreGive(stateMutex_);
  xSemaphoreGive(diskMutex_);
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
  // Bounded repair loop: a save() racing the write leaves newer state owed
  // and the file momentarily behind the mirror — rewrite it immediately
  // instead of waiting for the next interval gate (Copilot, PR #112).
  for (int attempt = 0; attempt < kMaxFlushAttempts; ++attempt) {
    Record snapshot = {};
    char cachePath[sizeof(cachePath_)] = {};
    uint32_t sinceFlushSec = 0;
    uint32_t generation = 0;
    bool hasWork = false;
    {
      xSemaphoreTake(stateMutex_, portMAX_DELAY);
      if (!bookOpen_ || cachePath_[0] == '\0') {
        writeQueued_ = false;
      } else if (*current_ == *lastFlushed_) {
        writeQueued_ = false;
      } else {
        snapshot = *current_;
        memcpy(cachePath, cachePath_, sizeof(cachePath));
        generation = bookGeneration_;
        sinceFlushSec = secsSinceFlush(lastFlushSec_);
        hasWork = true;
      }
      xSemaphoreGive(stateMutex_);
    }
    if (!hasWork) {
      LOG_DBG(MUTEX_TAG, "flushChanged(): nothing to flush");
      return true;  // nothing to do
    }

    LOG_INF(MUTEX_TAG, "flushChanged(): flushing spine=%u page=%u/%u sinceFlush=%lus", snapshot.spineIndex,
            snapshot.pageNumber, snapshot.pageCount, static_cast<unsigned long>(sinceFlushSec));
    const bool ok = commitRecord(cachePath, snapshot, /*adopt=*/false, generation);
    if (!ok) {
      // Transient SD failures retry within the bounded loop (CodeRabbit,
      // PR #112): writeQueued_ stays set either way, and the exit check
      // below reports failure while the record remains uncommitted.
      LOG_ERR(MUTEX_TAG, "Progress save FAILED: spine=%u page=%u/%u (attempt %d)", snapshot.spineIndex,
              snapshot.pageNumber, snapshot.pageCount, attempt + 1);
      continue;
    }
    bool caughtUp = false;
    {
      xSemaphoreTake(stateMutex_, portMAX_DELAY);
      caughtUp = !bookOpen_ || *current_ == *lastFlushed_;
      xSemaphoreGive(stateMutex_);
    }
    if (caughtUp) {
      LOG_INF(MUTEX_TAG, "Progress saved: spine=%u page=%u/%u", snapshot.spineIndex, snapshot.pageNumber,
              snapshot.pageCount);
      return true;
    }
    // A save raced the write: loop to repair the file with the newest state
    // before declaring the flush complete.
  }
  // Exit state decides the verdict (CodeRabbit, PR #112): false while the
  // newest record is still owed (persistent write failure, or retry budget
  // exhausted with newer state queued). Callers closeBook()/flushNow() treat
  // false as "not committed" instead of assuming success.
  bool caughtUp = false;
  {
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    caughtUp = !bookOpen_ || *current_ == *lastFlushed_;
    xSemaphoreGive(stateMutex_);
  }
  if (caughtUp) {
    LOG_INF(MUTEX_TAG, "flushChanged(): newer state remains queued (converged)");
    return true;
  }
  LOG_INF(MUTEX_TAG, "flushChanged(): record remains uncommitted");
  return false;
}

bool ProgressManager::commitRecord(const char* cachePath, const Record& rec, const bool adopt,
                                   const uint32_t generation) {
  // Lock order diskMutex_ → stateMutex_ (no path holds stateMutex_ across a
  // diskMutex_ acquire, so no inversion). Holding diskMutex_ across the
  // baseline commit makes "disk write + memory baseline" indivisible: two
  // concurrent writers finish in a total order, and disk + memory end up
  // describing the same record.
  xSemaphoreTake(diskMutex_, portMAX_DELAY);
  // Session check BEFORE the disk write (Copilot, PR #113): a worker that
  // snapshotted before closeBook() must not land a stale record on a file a
  // reopened book may be about to read. Only the deferred flush path is
  // validated — adopt=true bypass saves are synchronous and authoritative by
  // contract, and may legitimately target a different book (KOReader sync of
  // the previous book while reading). closeBook()/openBook() mutate session
  // state under diskMutex_ too, so the verdict cannot go stale between here
  // and the write.
  if (!adopt) {
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    const bool active = bookOpen_ && strncmp(cachePath_, cachePath, sizeof(cachePath_)) == 0 &&
                        (generation == 0 || bookGeneration_ == generation);
    xSemaphoreGive(stateMutex_);
    if (!active) {
      xSemaphoreGive(diskMutex_);
      LOG_DBG(MUTEX_TAG, "commitRecord(): session ended — dropping stale snapshot");
      return true;  // nothing owed to the closed session
    }
  }
  const bool ok = saveRecord(cachePath, rec);
  if (ok) {
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    const bool sameBook = bookOpen_ && strncmp(cachePath_, cachePath, sizeof(cachePath_)) == 0;
    if (sameBook) {
      if (*current_ == rec) {
        *lastFlushed_ = rec;
        lastFlushSec_ = static_cast<uint32_t>(millis() / 1000);
        writeQueued_ = false;
      } else if (adopt) {
        // Authoritative bypass save: adopt the record as the live mirror so a
        // later flush cannot overwrite the just-written file with an older
        // position (KOReader sync / cache-clear guarantee).
        *current_ = rec;
        *lastFlushed_ = rec;
        lastFlushSec_ = static_cast<uint32_t>(millis() / 1000);
        writeQueued_ = false;
      } else {
        // Stale worker snapshot: a newer save raced the write. Keep the newer
        // state owed; flushChanged()'s repair loop writes it immediately.
        writeQueued_ = true;
      }
    }
    xSemaphoreGive(stateMutex_);
  }
  xSemaphoreGive(diskMutex_);
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
  const size_t bytesToRead = fileSize > progress_record::kSizeGeneration ? progress_record::kSizeGeneration + 1
                                                                         : static_cast<size_t>(fileSize);
  const int n = f.read(data, bytesToRead);
  ProgressRecord rec;
  const size_t size =
      n == static_cast<int>(bytesToRead) ? progress_record::decode(data, static_cast<size_t>(n), rec) : 0;
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