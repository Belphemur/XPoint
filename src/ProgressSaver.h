#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../lib/ProgressFlush/ProgressFlush.h"

// Background progress flusher for the EPUB reader
// (docs/design/2026-09-10-progress-save-timer.md). The reader task captures
// the current position on every page turn (pure memory); a low-priority task
// writes the record to <cachePath>/progress.bin at most once per
// FLUSH_INTERVAL_MS when the position changed. Book exit, sleep and power-off
// call flushNow() to write synchronously on the caller's thread.
//
// The saver holds a COPY of the book's cache path, never the Epub object —
// the reader may release the epub before teardown (KOReader sync path), so a
// raw pointer would dangle. No book registered = flush is a no-op.
//
// Shared state is guarded by one mutex (review finding B1): on the dual-core
// S3 a bare portENTER_CRITICAL is per-core and not a cross-core exclusion
// pair. The mutex never wraps SD I/O; the record is copied out under the
// mutex and written outside it.
class ProgressSaver {
 public:
  static constexpr unsigned long FLUSH_INTERVAL_MS = 60000;

  ProgressSaver() = default;
  ~ProgressSaver();

  ProgressSaver(const ProgressSaver&) = delete;
  ProgressSaver& operator=(const ProgressSaver&) = delete;

  void begin();
  // Register the current book's cache dir. Empty path = no book (flushes no-op).
  void setBook(const char* cachePath);
  // Capture the reader's current position. Cheap, called from the reader
  // task on every position change; no SD I/O.
  void capture(uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount, bool hasOffset,
               uint32_t visibleTextOffset);
  // Synchronous flush of the pending record (if any). Called on book exit,
  // sleep entry and power-off. Returns true when nothing was left unwritten.
  bool flushNow();
  // THE synchronous progress save: writes `record` to `cachePath` right now
  // and records it as lastFlushed so the background tick never rewrites it.
  // Every save that bypasses the background task (low-battery per-turn,
  // footnote exit, DELETE_CACHE, KOReader sync) MUST go through here — the
  // saver is the single writer of progress state (design §4.7).
  bool saveNow(const char* cachePath, uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount, bool hasOffset,
               uint32_t visibleTextOffset);
  // Periodic flush attempt from the saver task.
  void flushTick();

  bool shouldFlush() const;

 private:
  bool writePending();

  SemaphoreHandle_t mutex_ = nullptr;
  ProgressFlush::FlushState state_;
  char cachePath_[160] = {0};
};

// Global instance (created at boot, fed by the EPUB reader activity).
extern ProgressSaver progressSaver;
