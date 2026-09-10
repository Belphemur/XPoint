#pragma once

#include <HalPowerManager.h>
#include <HalStorage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Single owner of EPUB reading progress
// (docs/design/2026-09-10-progress-save-timer.md):
// - FORMAT: the progress.bin record is encoded/decoded only here
//   (private saveRecord()/load()); the reader does no byte parsing.
// - STATE: the current position AND the last-flushed baseline live here
//   (heap-allocated; PSRAM on boards that have it). The reader reports
//   every page change via save(); the manager decides what reaches disk.
// - WRITES: gated — a change is persisted when FLUSH_INTERVAL_MS elapsed
//   since the last flush (or low battery); the write runs on the manager
//   task (core 0) so the reader never blocks on SD in steady state. Book
//   exit (closeBook) and sleep/power-off (flushNow) flush synchronously.
//
// The manager holds a COPY of the book's cache path, never the Epub object
// — the reader may release the epub before teardown (KOReader sync path),
// so a raw pointer would dangle. No book open = writes are no-ops.
//
// One mutex guards all state AND each disk write (single writer by
// construction: the worker and the synchronous flush paths can never run
// two writeAtomic() calls against the same file concurrently).
class ProgressManager {
 public:
  static constexpr unsigned long FLUSH_INTERVAL_MS = 60000;
  // On-disk record: 6 bytes of spine/page/count (+4 bytes visibleTextOffset
  // when known). Byte order matches the firmware's historical layout — do
  // not reorder without a load-side migration. The offset participates in
  // change detection: a same-page re-layout that only shifts the offset
  // still counts as progress.
  static constexpr size_t RECORD_SIZE_BASE = 6;
  static constexpr size_t RECORD_SIZE_OFFSET = 10;

  // Decoded progress.bin record.
  struct Record {
    uint16_t spineIndex = 0;
    uint16_t pageNumber = 0;
    uint16_t pageCount = 0;
    uint32_t visibleTextOffset = 0;
    bool hasOffset = false;

    bool operator==(const Record&) const = default;
  };

  ProgressManager() = default;
  ~ProgressManager();

  ProgressManager(const ProgressManager&) = delete;
  ProgressManager& operator=(const ProgressManager&) = delete;

  void begin();
  // Book open: loads the on-disk record (if valid) as BOTH the current
  // progress and the last-flushed baseline, and registers the cache dir.
  // Returns true when a record was loaded (out fields filled, hasOffset
  // implied by the record size); false = fresh book (outs are 0).
  bool openBook(const char* cachePath, uint16_t& spineIndex, uint16_t& pageNumber, uint16_t& pageCount,
                uint32_t& visibleTextOffset);
  // The reader calls this on EVERY page change: the in-memory position is
  // always up to date; the disk write is queued to the manager task only
  // when the gate allows (changed + interval elapsed, or low battery).
  void save(uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount, bool hasOffset, uint32_t visibleTextOffset);
  // Forced synchronous save for bypass paths (KOReader sync, DELETE_CACHE):
  // writes `record` to `cachePath` now, updates the in-memory state to
  // match. Works even with no book open (explicit cache path).
  bool saveNow(const char* cachePath, uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount, bool hasOffset,
               uint32_t visibleTextOffset);
  // Book exit: synchronous flush of any unflushed change (bounded by one
  // record write), then full state reset. Called by the reader destructor —
  // exit flushing is the manager's job, not the activity's.
  void closeBook();
  // Sleep / power off / explicit exit flush: synchronous flush of any
  // unflushed change, state kept. Safe to call with no book open (no-op).
  bool flushNow();

 private:
  // ---- record format: the only encoder/decoder lives here ----

  // Read the progress record for `cachePath`. Returns the byte count read
  // (0 = no/garbage record) and fills the outs; visibleTextOffset only
  // meaningful when RECORD_SIZE_OFFSET is returned.
  static size_t load(const char* cachePath, uint16_t& spineIndex, uint16_t& pageNumber, uint16_t& pageCount,
                     uint32_t& visibleTextOffset);
  // Encode + write the record atomically. The single producer of the
  // on-disk byte layout (internal flush paths and EpubReaderUtils's
  // convenience wrapper route through here).
  static bool saveRecord(const char* cachePath, uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount,
                         bool hasOffset, uint32_t visibleTextOffset);

  // Mutex held: write current_ when it differs from lastFlushed_. Returns
  // true when nothing needed writing or the write succeeded.
  bool flushChangedLocked();

  // Low-battery threshold (design §4.5): below this (gauge HEALTHY, not
  // charging) save() stops gating on the interval — every change is
  // persisted as soon as the worker can.
  static constexpr uint8_t LOW_BATTERY_PERCENT = 5;
  // Gate-time low-battery query: the HAL's cached reading (BATTERY_POLL_MS
  // cadence) makes this cheap; reads only the battery singleton — static.
  static bool lowBattery();

  SemaphoreHandle_t mutex_ = nullptr;
  // Heap-allocated state (poolMalloc: PSRAM-backed on BOARD_HAS_PSRAM
  // boards, DRAM otherwise). Null = uninitialized (begin() failed): all
  // ops no-op.
  // Heap-allocated state (poolMalloc: PSRAM-backed on BOARD_HAS_PSRAM
  // boards, DRAM otherwise). Null = uninitialized (begin() failed): all
  // ops no-op.
  Record* current_ = nullptr;
  Record* lastFlushed_ = nullptr;
  unsigned long lastFlushMs_ = 0;  // last successful disk flush (millis())
  char cachePath_[160] = {0};
  bool bookOpen_ = false;
  bool writeQueued_ = false;  // worker owes a write
  TaskHandle_t worker_ = nullptr;
};

// Global instance (created at boot, fed by the EPUB reader activity).
extern ProgressManager progressManager;
