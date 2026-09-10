#pragma once

#include <cstdint>

// Host-testable state machine behind the reader's background progress flusher
// (docs/design/2026-09-10-progress-save-timer.md). Pure C++: no FreeRTOS, no
// SD, no Arduino — the device wrapper (src/ProgressSaver) supplies the task,
// mutex and write; tests run the state machine on the host.
namespace ProgressFlush {

// Mirrors the on-disk progress.bin record (EpubReaderUtils save/load: 6 bytes
// of spine/page/count, +4 bytes visibleTextOffset when known). Field order
// matches the file layout; the offset participates in change detection so a
// same-page re-layout that only shifts the offset still counts as progress.
struct Record {
  uint16_t spineIndex = 0;
  uint16_t pageNumber = 0;
  uint16_t pageCount = 0;
  uint32_t visibleTextOffset = 0;
  bool hasOffset = false;

  bool operator==(const Record&) const = default;
};

// Dirty/latch state machine. Callers (device side) serialize access with a
// mutex — the reader task captures, the flusher task flushes.
class FlushState {
 public:
  // Capture a new position. Returns true when the record differs from both
  // the last flushed record and any pending (unflushed) one — i.e. a flush
  // is now owed. Equal captures are no-ops: a reader parked on one page
  // never becomes dirty.
  bool capture(const Record& record) {
    if (!dirty_ && flushed_ && record == lastFlushed_) return false;
    if (dirty_ && record == pending_) return false;
    pending_ = record;
    dirty_ = true;
    return true;
  }

  // Take the pending record for writing. Returns false when nothing is
  // pending; clears dirty until endFlush() decides the outcome.
  bool beginFlush(Record& out) {
    if (!dirty_) return false;
    out = pending_;
    dirty_ = false;
    return true;
  }

  // Complete a flush begun with beginFlush(). Success: the written record
  // becomes the new lastFlushed. Failure: re-arm the dirty flag — either a
  // newer capture already set it (setting again is harmless) or the same
  // record must be retried. pending_ always holds the newest capture, so a
  // retry never resurrects a stale record.
  void endFlush(const Record& written, bool ok) {
    if (ok) {
      lastFlushed_ = written;
      flushed_ = true;
      return;
    }
    dirty_ = true;
  }

  // Record that `record` was already written by an external synchronous save
  // (KOReader sync, DELETE_CACHE). Clears the dirty flag only when no newer
  // capture intervened; otherwise the newer capture still owes a write.
  void markFlushed(const Record& record) {
    if (dirty_ && !(record == pending_)) return;
    lastFlushed_ = record;
    flushed_ = true;
    dirty_ = false;
  }

  // Drop any pending record without writing. Only used when the book
  // context changes (setBook("")); a pending record from book A must never
  // be written into book B's cache dir. A concurrent failed write can still
  // re-arm the flag afterwards; the device wrapper treats "no book" as
  // written, so the retry self-cancels.
  void clearPending() { dirty_ = false; }

  bool shouldFlush() const { return dirty_; }
  bool hasFlushed() const { return flushed_; }
  const Record& lastFlushed() const { return lastFlushed_; }

 private:
  bool dirty_ = false;
  bool flushed_ = false;  // any record ever flushed (guards the first capture)
  Record lastFlushed_{};
  Record pending_{};
};

}  // namespace ProgressFlush
