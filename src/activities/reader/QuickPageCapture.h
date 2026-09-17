#pragma once

// QuickPageCapture — deep-copies one engine Page so it can be painted after
// the transient ChapterLayout pass that produced it. Engine page data dies
// when PageSink::onPage() returns (the per-page arena resets), so the quick
// font sheet's paint-once preview must carry the candidate page out of the
// callback: only the last scanned page is ever displayed, and painting every
// scanned page made each +/- tap pay N-1 invisible page paints (issue #137).
//
// Page strings (run text, link targets/fragments, image hrefs, ruby text)
// all live in the engine's per-page arena; each is bumped into the capture
// buffer and the record's pointer is rebased to the copy. Record arrays are
// sized for the STANDARD build profile; a page that does not fit returns
// false and the caller paints inline instead.

#include <cstddef>
#include <cstdint>

#include "layout/ChapterLayout.h"  // Page + record types

namespace {
constexpr size_t alignUp(size_t offset, size_t alignment) { return (offset + alignment - 1) & ~(alignment - 1); }
}  // namespace

class QuickPageCapture {
 private:
  static constexpr uint16_t kMaxRuns = 768;   // STANDARD profile kMaxRunsPerPage
  static constexpr uint16_t kMaxLinks = 24;   // STANDARD profile kMaxLinksPerPage
  static constexpr uint16_t kMaxImages = 16;  // STANDARD profile kMaxImagesPerPage
  static constexpr uint16_t kMaxRules = 16;   // STANDARD profile kMaxRulesPerPage
  static constexpr uint16_t kMaxRubies = 24;  // STANDARD profile kMaxRubiesPerPage
  // Page strings come from the engine's per-page arena (24 KB cap on the
  // STANDARD profile); the copy region matches with slack.
  static constexpr size_t kStringCap = 28 * 1024;

 public:
  // Fixed buffer layout: record arrays first (typed offsets), then the
  // string bump region. Offsets are 8-aligned so pointer-sized members of
  // every record are aligned on both 32-bit firmware and 64-bit hosts.
  static constexpr size_t kRunsOff = 0;
  static constexpr size_t kLinksOff = alignUp(kRunsOff + kMaxRuns * sizeof(freeink::book::PageTextRun), 8);
  static constexpr size_t kImagesOff = alignUp(kLinksOff + kMaxLinks * sizeof(freeink::book::PageLink), 8);
  static constexpr size_t kRulesOff = alignUp(kImagesOff + kMaxImages * sizeof(freeink::book::PageImage), 8);
  static constexpr size_t kRubiesOff = alignUp(kRulesOff + kMaxRules * sizeof(freeink::book::PageRule), 8);
  static constexpr size_t kStringsOff = alignUp(kRubiesOff + kMaxRubies * sizeof(freeink::book::PageRuby), 8);

  // Total backing-buffer size (record arrays + string region). One
  // allocation serves every capture until the sheet closes.
  static constexpr size_t kBufferBytes = kStringsOff + kStringCap;

  // Installs the backing buffer (one allocation, reused across captures).
  // Must be max-aligned (malloc/poolMalloc) — the record arrays sit at
  // fixed offsets from it.
  void attach(uint8_t* buffer, size_t cap) {
    buffer_ = buffer;
    cap_ = cap;
  }
  bool attached() const { return buffer_ != nullptr; }

  // Drops the captured page (the backing buffer stays attached).
  void reset() { valid_ = false; }

  // Deep-copies `page` into the buffer, overwriting any previous capture.
  // Returns false when the page does not fit; the capture state is then
  // unusable until the next successful capture().
  bool capture(const freeink::book::Page& page);

  // True when the last capture() succeeded.
  bool ready() const { return valid_; }

  // The captured page. Valid only while `ready()` and no capture()/reset()
  // has run since.
  const freeink::book::Page& page() const { return page_; }

 private:
  // Copies a NUL-terminated string into the bump region and returns the
  // copy (nullptr when the region is exhausted).
  const char* copyString(const char* src);

  uint8_t* buffer_ = nullptr;
  size_t cap_ = 0;
  size_t stringUsed_ = 0;
  bool valid_ = false;
  freeink::book::Page page_{};
};
