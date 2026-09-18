// FibpPrefetchWorker — see FibpPrefetchWorker.h for the threading model.

#if defined(CROSSPOINT_TTF_READER)

#include "FibpPrefetchWorker.h"

#if FIBP_WORKER_ENABLED

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include "ChapterIndexEngine.h"

namespace freeink {
namespace book {

// ── faces (main thread only — FT face create/destroy discipline) ────────────

bool FibpPrefetchWorker::buildFaces() {
  // Resolve the same family the loader resolves for the reader chain
  // (§14.4 manifest is boot-stable). An unknown/unavailable family leaves
  // the worker inert: the sync path already serves that case.
  const FamilyInfo* fam = familyName_[0] != '\0' ? fontLoader.findFamily(familyName_) : nullptr;
  if (fam == nullptr || !BookFontLoader::isFamilyAvailable(*fam)) {
    LOG_DBG("PREF", "No worker family '%s' — worker inert", familyName_);
    return false;
  }

  // Mirror tryLoadFace's PSRAM tier: PSRAM-only byte copy per face, the
  // loader's per-face size guard, then FT face init over the borrowed
  // bytes. Slot order and content match the loader's, so the derived
  // fingerprint (and with it the FIBP generation) is identical.
  bool anyLoaded = false;
  uint32_t h = 0x811c9dc5;
  for (uint8_t i = 0; i < 4 && i < fam->faceCount; ++i) {
    const FontFaceInfo& fi = fam->faces[i];
    if (fi.fileSize == 0 || fi.fileSize > BookFontLoader::kMaxFaceBytes) continue;
    fontBytes_[i] = poolMakeBytes(fi.fileSize);
    if (!fontBytes_[i]) {
      LOG_ERR("PREF", "PSRAM OOM: %u bytes for %s", static_cast<unsigned>(fi.fileSize), fi.file);
      break;  // fewer faces → fingerprint drift → FIBP names miss (harmless)
    }
    auto* bytes = static_cast<uint8_t*>(fontBytes_[i].get());
    HalFile file;
    if (!Storage.openFileForRead("PREF", fi.file, file) || file.read(bytes, fi.fileSize) != fi.fileSize) {
      LOG_ERR("PREF", "Font read failed: %s", fi.file);
      fontBytes_[i].reset();
      continue;
    }
    if (!BookFontLoader::validateSfntBytes(bytes, fi.fileSize)) {
      fontBytes_[i].reset();
      continue;
    }
    auto* face = new (std::nothrow) NativeFace();
    if (face == nullptr) {
      LOG_ERR("PREF", "OOM: face for %s", fi.file);
      fontBytes_[i].reset();
      continue;
    }
    if (!face->init(bytes, fi.fileSize, BookFontLoader::kInitSizePx, (fi.styleFlags & StyleBold) ? 700 : 400,
                    (fi.styleFlags & StyleItalic) != 0)) {
      LOG_ERR("PREF", "Face init failed: %s", fi.file);
      delete face;
      fontBytes_[i].reset();
      continue;
    }
    if (!chain_.add(face, fi.styleFlags)) {
      LOG_ERR("PREF", "Chain add failed: %s", fi.file);
      delete face;
      fontBytes_[i].reset();
      continue;
    }
    faces_[i] = face;
    h = BookFontLoader::fontBytesHash(bytes, fi.fileSize, h);
    anyLoaded = true;
  }
  if (!anyLoaded) return false;

  // Fingerprint parity: the loader hashes bytes and xors the chain coverage
  // BEFORE appending the fallback tail (computeFingerprint() runs first in
  // ensureLoaded()), so the worker folds coverage at the same point.
  h ^= static_cast<uint32_t>(chain_.styleCoverage());
#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
  h ^= 0x46545531u;  // backend tag, mirrors computeFingerprint()
#endif
  fingerprint_ = h;
  // The tail registers the missing styles so coverage matches the reader's
  // chain; its faces are device-lifetime singletons whose metrics queries
  // are pure reads — safe to consult from the worker's layout pass.
  BookFontLoader::appendFallbackTail(chain_);
  return chain_.styleCoverage() != 0;
}

void FibpPrefetchWorker::teardownFaces() {
  for (auto*& face : faces_) {
    delete face;  // FT_Done_Face — main thread, after the join
    face = nullptr;
  }
  chain_ = FontChain{};
  for (auto& bytes : fontBytes_) bytes.reset();
  fingerprint_ = 0;
}

// ── params snapshot (scalars only; pointers stay worker-owned) ──────────────

void FibpPrefetchWorker::copyScalarParams(const LayoutParams& from) {
  params_.pageWidth = from.pageWidth;
  params_.pageHeight = from.pageHeight;
  params_.marginLeft = from.marginLeft;
  params_.marginRight = from.marginRight;
  params_.marginTop = from.marginTop;
  params_.marginBottom = from.marginBottom;
  params_.baseSizePx = from.baseSizePx;
  params_.lineSpacingPct = from.lineSpacingPct;
  params_.paragraphSpacingPct = from.paragraphSpacingPct;
  params_.defaultAlign = from.defaultAlign;
  params_.orphanLines = from.orphanLines;
  params_.widowLines = from.widowLines;
  params_.embeddedStyles = from.embeddedStyles;
  params_.focusReading = from.focusReading;
  params_.language = language_;
  params_.font = &chain_;
  params_.stylesheet = nullptr;
  params_.hyphenator = nullptr;
}

// ── lifecycle (main thread) ────────────────────────────────────────────────

bool FibpPrefetchWorker::begin(const BeginContext& ctx) {
  if (running_.load(std::memory_order_acquire)) return true;  // already active
  if (ctx.spineCount == 0) return false;

  cancel_.store(false, std::memory_order_release);
  snprintf(epubPath_, sizeof(epubPath_), "%s", ctx.epubPath);
  snprintf(cacheDir_, sizeof(cacheDir_), "%s", ctx.cacheDir);
  snprintf(familyName_, sizeof(familyName_), "%s", ctx.familyName);
  snprintf(language_, sizeof(language_), "%s", ctx.layout.language != nullptr ? ctx.layout.language : "en");
  spineCount_ = ctx.spineCount;
  gen_.store(ctx.generation, std::memory_order_release);
  notifiedSpine_.store(fibp::kNoChapter, std::memory_order_release);
  copyScalarParams(ctx.layout);
  sessionGen_ = ctx.generation;

  if (!buildFaces()) {
    teardownFaces();
    LOG_DBG("PREF", "Worker inert (no face set) — sync path unchanged");
    return false;
  }
  params_.font = &chain_;

  queue_.clear();
  queue_.reserve(spineCount_);
  failed_.assign(spineCount_, 0);
  queueCursor_ = 0;

  // The runtime opens on the worker task (catalog work is heavy SD I/O).
  runtime_ = makeUniqueNoThrow<TtfBookRuntime>();
  if (runtime_ == nullptr) {
    LOG_ERR("PREF", "OOM: TtfBookRuntime");
    teardownFaces();
    return false;
  }

  exitedSem_ = xSemaphoreCreateBinary();
  if (exitedSem_ == nullptr) {
    LOG_ERR("PREF", "OOM: exit semaphore");
    runtime_.reset();
    teardownFaces();
    return false;
  }
  paramsMux_ = xSemaphoreCreateMutex();
  if (paramsMux_ == nullptr) {
    LOG_ERR("PREF", "OOM: params mutex");
    vSemaphoreDelete(exitedSem_);
    exitedSem_ = nullptr;
    runtime_.reset();
    teardownFaces();
    return false;
  }

  running_.store(true, std::memory_order_release);
  if (xTaskCreatePinnedToCore(taskTrampoline, "fibpprefetch", kStackBytes, this, kPriority, &task_, kCore) != pdPASS) {
    LOG_ERR("PREF", "Task spawn failed");
    running_.store(false, std::memory_order_release);
    vSemaphoreDelete(exitedSem_);
    exitedSem_ = nullptr;
    task_ = nullptr;
    runtime_.reset();
    teardownFaces();
    return false;
  }
  LOG_INF("PREF", "Worker started: %u spines, gen=%08x", static_cast<unsigned>(spineCount_), ctx.generation);
  return true;
}

void FibpPrefetchWorker::notifyChapterEntered(const uint16_t spine) {
  notifiedSpine_.store(spine, std::memory_order_release);
}

void FibpPrefetchWorker::notifyGeneration(const uint32_t generation, const LayoutParams& pods) {
  // The generation lands first: the yield hook aborts the in-flight session
  // at the next chunk boundary, then the pass re-seeds under the new value.
  gen_.store(generation, std::memory_order_release);
  if (paramsMux_ != nullptr && xSemaphoreTake(paramsMux_, pdMS_TO_TICKS(kJoinTimeoutMs)) == pdTRUE) {
    copyScalarParams(pods);
    xSemaphoreGive(paramsMux_);
  }
  // Respawn after a "fully indexed" self-exit so later settings changes can
  // re-index under the new generation.
  if (!running_.load(std::memory_order_acquire)) {
    if (exitedSem_ != nullptr) {
      vSemaphoreDelete(exitedSem_);
      exitedSem_ = nullptr;
    }
    exitedSem_ = xSemaphoreCreateBinary();
    if (exitedSem_ == nullptr) {
      LOG_ERR("PREF", "OOM: respawn semaphore");
      return;
    }
    running_.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(taskTrampoline, "fibpprefetch", kStackBytes, this, kPriority, &task_, kCore) !=
        pdPASS) {
      LOG_ERR("PREF", "Task respawn failed");
      running_.store(false, std::memory_order_release);
      vSemaphoreDelete(exitedSem_);
      exitedSem_ = nullptr;
      task_ = nullptr;
    }
  }
}

void FibpPrefetchWorker::cancel() {
  cancel_.store(true, std::memory_order_release);
  if (running_.load(std::memory_order_acquire) && exitedSem_ != nullptr) {
    if (xSemaphoreTake(exitedSem_, pdMS_TO_TICKS(kJoinTimeoutMs)) != pdTRUE) {
      // Pathological: the worker is wedged inside a step. Leak the task and
      // its state rather than free memory it still touches (the book-close
      // path degrades to a logged leak instead of a use-after-free).
      LOG_ERR("PREF", "Join timeout — leaking worker resources");
      return;
    }
  }
  if (exitedSem_ != nullptr) {
    vSemaphoreDelete(exitedSem_);
    exitedSem_ = nullptr;
  }
  if (paramsMux_ != nullptr) {
    vSemaphoreDelete(paramsMux_);
    paramsMux_ = nullptr;
  }
  task_ = nullptr;
  if (runtime_ != nullptr) runtime_->close();
  runtime_.reset();
  teardownFaces();
  queue_.clear();
  queue_.shrink_to_fit();
  failed_.clear();
  failed_.shrink_to_fit();
  sessionGen_ = 0;
  running_.store(false, std::memory_order_release);
  LOG_DBG("PREF", "Worker cancelled");
}

// ── worker task ────────────────────────────────────────────────────────────

void FibpPrefetchWorker::taskTrampoline(void* arg) { static_cast<FibpPrefetchWorker*>(arg)->run(); }

void FibpPrefetchWorker::run() {
  if (!runtime_->open(epubPath_, cacheDir_)) {
    LOG_ERR("PREF", "Runtime open failed — worker done");
    running_.store(false, std::memory_order_release);
    task_ = nullptr;
    if (exitedSem_ != nullptr) xSemaphoreGive(exitedSem_);
    vTaskDelete(nullptr);
  }

  uint16_t lastNotifiedSeen = notifiedSpine_.load(std::memory_order_acquire);
  for (;;) {
    if (cancel_.load(std::memory_order_acquire)) break;
    building_.store(fibp::kNoChapter, std::memory_order_release);
    const uint32_t gen = gen_.load(std::memory_order_acquire);
    if (gen != sessionGen_) {
      // Generation bump (settings change): retry everything under the new
      // generation; the old in-flight session was already aborted by the
      // yield hook.
      sessionGen_ = gen;
      failed_.assign(spineCount_, 0);
      queue_.clear();
      queueCursor_ = 0;
    }
    if (queue_.empty()) {
      replan(gen);
      lastNotifiedSeen = notifiedSpine_.load(std::memory_order_acquire);
    }
    uint16_t spine = fibp::kNoChapter;
    while (queueCursor_ < queue_.size()) {
      const uint16_t candidate = queue_[queueCursor_++];
      if (failed_[candidate]) continue;
      if (candidate == notifiedSpine_.load(std::memory_order_acquire)) {
        // The entered chapter belongs to the reader (its sync rebuild owns
        // the spine while it is on screen) — the worker skips it here and
        // re-plans later if it is still unwritten.
        continue;
      }
      // Claim BEFORE the cache check so the reader's handoff sees a stable
      // claim; the plan is re-verified after claiming to close the race
      // against a chapter entry landing between the two checks.
      building_.store(candidate, std::memory_order_release);
      if (candidate == notifiedSpine_.load(std::memory_order_acquire) || spineHasCache(candidate, gen)) {
        building_.store(fibp::kNoChapter, std::memory_order_release);
        continue;
      }
      spine = candidate;
      break;
    }
    if (spine == fibp::kNoChapter) {
      // Planned spines all exist or failed. A newer chapter entry reorders
      // the plan (the tail spines may now be unwritten); otherwise done.
      if (notifiedSpine_.load(std::memory_order_acquire) != lastNotifiedSeen) continue;
      break;  // fully indexed (attempted) — release the 24KB stack (R1)
    }

    // Heap floor (R1): wait instead of indexing into the reader's DRAM
    // budget; the wait also re-checks cancel/generation.
    while ((ESP.getFreeHeap() < kMinFreeHeap || ESP.getMaxAllocHeap() < kMinMaxAllocHeap) &&
           !cancel_.load(std::memory_order_acquire) && gen_.load(std::memory_order_acquire) == gen) {
      vTaskDelay(pdMS_TO_TICKS(kHeapWaitMs));
    }
    if (cancel_.load(std::memory_order_acquire) || gen_.load(std::memory_order_acquire) != gen) continue;

    const ChapterRun r = buildSpine(spine, gen);
    if (r == ChapterRun::Failed) failed_[spine] = 1;
    // Cancelled just falls through: the loop head re-seeds or exits.
    vTaskDelay(pdMS_TO_TICKS(kSpineDelayMs));
  }

  running_.store(false, std::memory_order_release);
  task_ = nullptr;
  if (exitedSem_ != nullptr) xSemaphoreGive(exitedSem_);
  vTaskDelete(nullptr);
}

void FibpPrefetchWorker::replan(const uint32_t generation) {
  // The generation argument documents the plan's context; the queue holds
  // spine indices only (the per-spine work re-reads the atomic).
  (void)generation;
  queue_.resize(spineCount_);
  fibp::buildQueue(spineCount_, notifiedSpine_.load(std::memory_order_acquire), queue_.data(),
                   static_cast<uint16_t>(spineCount_));
  queueCursor_ = 0;
}

bool FibpPrefetchWorker::spineHasCache(const uint16_t spine, const uint32_t generation) {
  // Complete AND partial files count: the reader holds/serves existing
  // caches, and renaming over an open reader cache is never attempted.
  const BookStatus st = runtime_->openChapterCache(spine, generation);
  if (st == BookStatus::Ok) {
    runtime_->closeChapterCache();
    return true;
  }
  return false;
}

ChapterRun FibpPrefetchWorker::buildSpine(const uint16_t spine, const uint32_t generation) {
  const TickType_t startTicks = xTaskGetTickCount();
  lastPages_ = 0;
  // Snapshot the params under the mutex: the engine copies them at begin,
  // and a concurrent notifyGeneration must not tear the scalar set.
  LayoutParams snapshot;
  if (paramsMux_ != nullptr) xSemaphoreTake(paramsMux_, portMAX_DELAY);
  snapshot = params_;
  if (paramsMux_ != nullptr) xSemaphoreGive(paramsMux_);
  const ChapterIndexYield yield{this, &FibpPrefetchWorker::yieldHook};
  const ChapterRun r = runChapterBuild(*runtime_, spine, snapshot, generation, yield, /*chunkPages=*/1);
  const uint32_t ms = (xTaskGetTickCount() - startTicks) * portTICK_PERIOD_MS;

  if (r == ChapterRun::Completed) {
    // Telemetry (R5): one line per indexed spine + the R1 stack probe.
    runtime_->openChapterCache(spine, generation);
    const uint32_t pages = runtime_->availablePageCount(spine);
    runtime_->closeChapterCache();
    char href[64] = {};
    runtime_->catalog().spineHref(spine, href, sizeof(href));
    LOG_INF("PREF", "indexed %s pages=%u ms=%u hwm=%u", href, static_cast<unsigned>(pages), static_cast<unsigned>(ms),
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  } else if (r == ChapterRun::Failed) {
    LOG_ERR("PREF", "Build failed s%u gen=%08x ms=%u", spine, generation, static_cast<unsigned>(ms));
  } else {
    LOG_DBG("PREF", "Build cancelled s%u (cancel/gen)", spine);
  }
  return r;
}

bool FibpPrefetchWorker::yieldHook(void* ctx, const uint16_t pagesBuilt) {
  auto* self = static_cast<FibpPrefetchWorker*>(ctx);
  self->lastPages_ = pagesBuilt;
  if (fibp::shouldStopChunk(self->cancel_.load(std::memory_order_relaxed), self->gen_.load(std::memory_order_acquire),
                            self->sessionGen_)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(kPageDelayMs));  // R4: yield between pages
  return true;
}

}  // namespace book
}  // namespace freeink

#endif  // FIBP_WORKER_ENABLED
#endif  // CROSSPOINT_TTF_READER
