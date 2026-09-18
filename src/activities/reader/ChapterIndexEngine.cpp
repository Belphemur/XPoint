// ChapterIndexEngine — see ChapterIndexEngine.h.

#if defined(CROSSPOINT_TTF_READER)

#include "ChapterIndexEngine.h"

namespace freeink {
namespace book {

BookStatus ensureChapterSession(ChapterIndexTarget& target, const uint16_t spineIndex, const LayoutParams& params,
                                const uint32_t generation) {
  if (target.sessionFor(spineIndex)) return BookStatus::Ok;
  return target.beginChapterSession(spineIndex, params, generation);
}

ChapterPump pumpChapterChunk(ChapterIndexTarget& target, const uint16_t spineIndex, const uint16_t minNewPages,
                             BookStatus* stOut) {
  const BookStatus st = target.stepBuild(minNewPages);
  if (stOut != nullptr) *stOut = st;
  if (st == BookStatus::Ok) return target.sessionActive() ? ChapterPump::Progress : ChapterPump::Complete;
  // A soft failure keeps the session alive and remains pumpable; only a
  // torn-down session (no session state left for any spine) is fatal.
  if (target.sessionFor(spineIndex) || target.sessionActive() || target.sessionDone()) return ChapterPump::Progress;
  return ChapterPump::Failed;
}

ChapterRun runChapterBuild(ChapterIndexTarget& target, const uint16_t spineIndex, const LayoutParams& params,
                           const uint32_t generation, const ChapterIndexYield& yield, const uint16_t chunkPages) {
  const BookStatus begin = ensureChapterSession(target, spineIndex, params, generation);
  if (begin != BookStatus::Ok) return ChapterRun::Failed;

  for (;;) {
    if (yield.step != nullptr && !yield.step(yield.ctx, static_cast<uint16_t>(target.availablePageCount(spineIndex)))) {
      // Resumable partial commit; the caller re-seeds on the next run.
      target.abortSession();
      return ChapterRun::Cancelled;
    }
    switch (pumpChapterChunk(target, spineIndex, chunkPages)) {
      case ChapterPump::Progress:
        continue;
      case ChapterPump::Complete:
        return ChapterRun::Completed;
      case ChapterPump::Failed:
        return ChapterRun::Failed;
    }
    return ChapterRun::Failed;
  }
}

}  // namespace book
}  // namespace freeink

#endif  // CROSSPOINT_TTF_READER
