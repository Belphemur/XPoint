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
// Locking: stateMutex_ protects the in-memory record, book path, and flush
// bookkeeping. save() takes it only long enough to update native-width fields,
// never while touching SD. diskMutex_ separately serializes progress.bin I/O.
// A flush snapshots state, releases stateMutex_, then takes diskMutex_; after
// a successful write it advances lastFlushed_ only if current_ still matches
// that snapshot, so a concurrent save is retried instead of being overwritten.
class ProgressManager {
 public:
  static constexpr uint32_t FLUSH_INTERVAL_MS = 120000;
  // On-disk record: 6 bytes of spine/page/count (+4 bytes visibleTextOffset
  // when known). Byte order matches the firmware's historical layout — do
  // not reorder without a load-side migration. The offset participates in
  // change detection: a same-page re-layout that only shifts the offset
  // still counts as progress.
  static constexpr size_t RECORD_SIZE_BASE = 6;
  static constexpr size_t RECORD_SIZE_OFFSET = 10;
  // Retry budget for flushChanged(): how many times one call rewrites the
  // file when a racing save or a transient SD error leaves newer state owed.
  static constexpr uint8_t kMaxFlushAttempts = 4;

  // Decoded progress.bin record. The TTF reader (CROSSPOINT_TTF_READER
  // builds only) extends it with the FIBP generation tag; on PSRAM-less
  // builds the struct keeps its legacy shape.
  struct Record {
    uint16_t spineIndex = 0;
    uint16_t pageNumber = 0;
    uint16_t pageCount = 0;
    uint32_t visibleTextOffset = 0;
    bool hasOffset = false;
#if defined(CROSSPOINT_TTF_READER)
    // TTF record shape: charOffset (chapter char offset) + generation.
    // visibleTextOffset doubles as the charOffset carrier when hasGeneration
    // is set (hasOffset is false then) so the change-detection comparison
    // still sees position movement.
    bool hasGeneration = false;
    uint32_t generation = 0;
#endif

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
#if defined(CROSSPOINT_TTF_READER)
  // TTF-reader book open: same base restore, plus the generation-tagged
  // record fields when the 16-byte layout is on disk. Legacy (6/10-byte)
  // records degrade to a chapter-start restore (charOffset 0, generation 0).
  bool openBookTtf(const char* cachePath, uint16_t& spineIndex, uint16_t& pageNumber, uint16_t& pageCount,
                   uint32_t& charOffset, uint32_t& generation, bool& hasGeneration);
  // TTF reader position report: page.charStart + the generation it was laid
  // out under (same single-writer flush machinery as save()).
  void saveTtf(uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount, uint32_t charOffset, uint32_t generation);
#endif
  // The reader calls this on EVERY page change: the in-memory position is
  // always up to date; the disk write is queued to the manager task only
  // when the gate allows (changed + interval elapsed, or low battery).
  void save(uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount, bool hasOffset, uint32_t visibleTextOffset);
  // Forced synchronous save for bypass paths (KOReader sync, DELETE_CACHE):
  // writes `record` to `cachePath` now, updates the in-memory state to
  // match. Works even with no book open (explicit cache path). The TTF
  // overload appends the generation-tagged fields.
  bool saveNow(const char* cachePath, uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount, bool hasOffset,
               uint32_t visibleTextOffset);
#if defined(CROSSPOINT_TTF_READER)
  bool saveNowTtf(const char* cachePath, uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount,
                  uint32_t charOffset, uint32_t generation);
#endif
  // Shared body of the synchronous bypass saves (legacy + TTF record shapes).
  bool saveNowRecord(const char* cachePath, const Record& rec);
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
  // convenience wrapper route through here); the byte layout itself lives in
  // activities/reader/ProgressRecord.h.
  static bool saveRecord(const char* cachePath, const Record& rec);
  // diskMutex_ held: encode + write; callers of the public flush paths use
  // this so a read (openBook) and a write can never race on progress.bin.
  bool saveRecordLocked(const char* cachePath, const Record& rec);
  // One atomic save unit: disk write + in-memory baseline commit, serialized
  // on diskMutex_ so concurrent bypass saves cannot interleave with the
  // worker's flush. `adopt` marks a synchronous bypass save (KOReader sync,
  // cache-clear backup, footnote origin) as authoritative: when the live
  // mirror differs, the record replaces it instead of being reverted by a
  // later flush.
  bool commitRecord(const char* cachePath, const Record& rec, bool adopt);

  // Lock-free state reader: write current_ when it differs from
  // lastFlushed_; the disk write itself goes through saveRecordLocked().
  // Returns true when nothing needed writing or the write succeeded.
  bool flushChanged();

  // Low-battery threshold (design §4.5): below this (gauge HEALTHY, not
  // charging) save() stops gating on the interval — every change is
  // persisted as soon as the worker can.
  static constexpr uint8_t LOW_BATTERY_PERCENT = 5;
  // Gate-time low-battery query: the HAL's cached reading (BATTERY_POLL_MS
  // cadence) makes this cheap; reads only the battery singleton — static.
  static bool lowBattery();

  // Protects current_, lastFlushed_, cachePath_, bookOpen_, writeQueued_,
  // and lastFlushSec_. This mutex is never held across disk I/O.
  SemaphoreHandle_t stateMutex_ = nullptr;
  // Serializes every progress.bin disk access (load and saveRecord): an
  // openBook() read must never interleave with a flush's writeAtomic().
  // Null = begin() failed: all ops no-op (fail closed).
  SemaphoreHandle_t diskMutex_ = nullptr;
  // Heap-allocated state (poolMalloc: PSRAM-backed on BOARD_HAS_PSRAM
  // boards, DRAM otherwise). Null = uninitialized (begin() failed): all
  // ops no-op.
  Record* current_ = nullptr;
  Record* lastFlushed_ = nullptr;
  uint32_t lastFlushSec_ = 0;  // last successful disk flush (seconds since boot)
  char cachePath_[160] = {0};
  bool bookOpen_ = false;
  bool writeQueued_ = false;  // worker owes a write
  TaskHandle_t worker_ = nullptr;
  // Destructor handshake: set before waking the worker; the worker exits and
  // acknowledges on the semaphore below, so its loop can never touch freed
  // state after the destructor returns.
  SemaphoreHandle_t workerExit_ = nullptr;
  volatile bool workerStopping_ = false;
};

// Global instance (created at boot, fed by the EPUB reader activity).
extern ProgressManager progressManager;
