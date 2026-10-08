# Restore the reading position without a full reindex (TTF/FIBP path)

ISO date: 2026-10-08
Status: Design — pending implementation
Supersedes: `2026-10-04-preserve-reading-position-across-reindex.md` (kept for
the legacy-path analysis; its TTF changes are subsumed here, its legacy-path
Changes 3/4 are re-scoped as a follow-up, §8)
Relates to: #193 (short-chapter prefetch), #195 (position loss),
`2026-09-10-progress-save-timer.md` §3.5, `2026-09-27-quick-font-preview.md`

## 1. Problem

Two failure shapes, one root cause class. The reader must find the user's place
after ANY invalidation of the layout cache — a settings change while the book is
open, or a settings change while the book is closed and the book is reopened.
"If they were in the middle of a chapter, index everything until that moment"
and reopen there — never fall back to the chapter start while an anchor exists.

### 1.1 Reopen after a settings change → chapter start (position LOST)

`ttfResolveTargetPage()` (`EpubReaderActivity.cpp:2901-2905`):

```cpp
if (ttfHasSavedPosition) {
  if (currentSpineIndex != ttfSavedSpine || ttfSavedGeneration != ttfGeneration) {
    ttfHasSavedPosition = false;
    targetOut = 0;  // generation/spine mismatch: chapter-start degrade (§7)
    return true;
  }
```

The 16-byte `progress.bin` record already carries a layout-independent anchor
(`charOffset`, chapter character space) plus the `generation` it was saved
under. On reopen with a changed font/size/spacing the generation hash differs
(`layoutGenerationHash`, `PageCache.cpp:92`) and the record's anchor is
DISCARDED — the reader opens at chapter start, page 0. The saved page number is
meaningless across a generation bump, but the char offset is not; only the
resolve policy treats it as unusable.

### 1.2 Mid-session reflow → complete-index wait (position kept, slowly)

The reflow path (`ttfReflowJumpPending`, set by `ttfInvalidateCaches()` at
`:2741`) maps the live anchor through `canMapCompleteOffset()`
(`:2776-2780`), which requires `haveTotal` — session done or complete cache.
After an invalidation neither holds, so every pass takes `needFullBuild = true`
and the chapter indexes end to end before the reader resumes. The same
completeness gate hits KOReader offset jumps (`:2959-2980`) and TOC anchor
jumps (`:2821-2839`).

### 1.3 What the legacy Section engine already proves

`Section::buildReachedVisibleTextOffset()` (`Section.h:196`) + the reflow loop
(`EpubReaderActivity.cpp:2386-2419`) build exactly as far as the captured
visible-text offset — never to chapter end — then map exactly
(`getPageForVisibleTextOffset`). The TTF path must adopt the same semantics:
**build to the anchor, not to completeness.**

## 2. Goals

- An anchor (char offset) restores the position from ANY layout generation.
- Restoration requires only a prefix of the chapter index covering the anchor —
  never a complete index (except where semantically required: percent jumps,
  last-page sentinel).
- Restore UX: Indexing popup with a progress bar (`BaseTheme::drawProgressBar`)
  fed by input-side build progress. No page is painted until the anchor
  resolves (owner decision: block-to-anchor, no visual teleport).
- UI never feels dead: builds stay chunked per render pass; a navigation press
  during a restore supersedes it (§5.4).
- Zero changes to `progress.bin`'s on-disk format; `ProgressManager` stays the
  single writer. No migration needed — this is a resolve-policy change only.

## 3. Design

### 3.1 Principle: anchor authoritative, generation advisory

`generation` matching remains the FAST path (reuse the record's page number
directly on a matching-generation partial prefix — today's behavior at
`:2915-2920`, kept verbatim). A generation MISMATCH no longer degrades to
chapter start; it changes only WHICH mapping runs:

- same spine, matching generation, partial prefix → saved page number (fast);
- same spine, any generation → map `charOffset` through the built prefix
  (§3.2), building toward it as needed;
- different spine → chapter start (the anchor is chapter-scoped; nothing to
  map). Unchanged.

### 3.2 One resolver funnel (DRY)

The four near-duplicate mapping blocks in `ttfResolveTargetPage()` (reflow
`:2877`, saved position `:2901`, offset jump `:2959`, anchor `:2821`) collapse
into one policy evaluated against a unified "pending target" state:

```
struct PendingTarget {          // chapter-scoped, sentinel = none
  enum Kind : uint8_t { None, CharOffset, AnchorHash, Percent, LastPage } kind;
  uint32_t charOffset;          // CharOffset / AnchorHash-resolved
  uint32_t idHash;              // AnchorHash
  uint8_t  percent;             // Percent
};
```

Policy per pass, in priority order (superseding navigation already handled):

1. **Covered** — the built prefix covers the target (`coverCheck`, §3.3) →
   resolve to `pageForChar(target)` and clear the pending state.
2. **Chapter ends before the target** — session/cache complete and
   `target > totalChars` → clamp to the last page, clear pending (terminal; no
   pending-forever state).
3. **Not covered, not terminal** → keep pending, `needFullBuild = false`;
   the render pass builds ONE chunk toward it (§5.2). Percent and LastPage
   kinds keep `needFullBuild = true` (they cannot resolve from a prefix).

Saved-position restore, mid-session reflow, and KOReader offset jumps all
produce a `CharOffset` target (from `ttfSavedCharOffset`, `ttfCurrentCharStart`,
`*pendingOffsetJump` respectively). TOC/bookmark anchors produce `AnchorHash`
(resolved to a char offset via `charForAnchor` first, then the same policy).

### 3.3 Cover check — the watermark-ambiguity rule

`PageCacheReader::pageForChar()` returns the last page with
`charStart <= offset` and has NO failure signal (reference:
`reader-position-and-reindex.md` — "verify SDK accessors return what their
signature implies"). An offset past the watermark silently maps to the LAST
BUILT page. The cover check must therefore compare against the page's own
charStart range and the prefix watermark:

```
covered(target):
  p = pageForChar(target)
  last = availablePageCount() - 1
  if complete (session done or committed full cache):   return p, exact
  if p < last:                                          return p, exact
  if p == last && target == charStart(last):            return p, exact
  return not covered        // p == last && target > charStart(last):
                            // clamped watermark hit — ambiguous
```

On "not covered" the pending state is NEVER consumed (peek, don't consume —
consuming here would land the reader at the watermark page and look like a
position loss). The build advances one chunk and the next pass re-checks.
This is the exact trap `reader-position-and-reindex.md` §"Scoping a one-shot
value" documents as "consumed before it is servable".

### 3.4 Restore UI: Indexing popup + the theme's small progress bar

While a pending restore owns the pass, the reader repaints the themed Indexing
popup with the same pair the home screen uses for cover generation
(`HomeActivity.cpp:144-219`):

- `const Rect popupRect = GUI.drawPopup(renderer, tr(STR_INDEXING));` — the
  returned layout rect is kept for the pass;
- `GUI.fillPopupProgress(renderer, popupRect, percent)` — the small
  metric-driven bar INSIDE the popup frame (`BaseTheme.cpp:795`:
  `popupProgressBarHeight`, optional outline, `popupProgressClampPercent`),
  which issues its own `FAST_REFRESH`. No percent text, no separate rect math.

Progress basis: **input-side** build progress — available before the first
page exists and monotonic:

- inline session: `sessionBytesConsumed()` / `sessionBytesTotal()`;
- resumed partial: `cacheBuildBytesConsumed()` / `cacheBuildBytesTotal()`
  (`PageCacheReader`, `TtfBookRuntime.h:81-82`).

`percent = consumed * 100 / total` (the metric's clamp guards overflow at the
edges; 0 on a not-yet-started session). The bar may END BELOW 100% — when the
anchor resolves the wait is over because the position was reached, not because
the chapter finished. The bar measures chapter progress toward your place;
disappearing at 45% is the honest signal.

### 3.5 During a pending restore, no page is painted

Block-to-anchor (owner decision): the readable-page gate and the
`shouldClearBuildPopup()` yield (`MIN_PAGES_TO_CLEAR_POPUP`) do NOT fire while
a restore target is pending — painting page 0 of a partially built chapter
would visually announce a position loss even though the resolve later corrects
it. The popup IS the painted content until the anchor resolves. Normal
(forward reading) indexing keeps today's yield behavior unchanged.

## 4. State scoping (the whole set, at the chokepoint)

`clearDeferredReposition()` (`:4395`) remains the single chokepoint where
chapter-scoped restore state dies. Its member set is extended to the unified
pending state and its invariant comment updated to enumerate ALL members —
per `reader-position-and-reindex.md` §"enumerate the WHOLE set":

- `PendingTarget` (kind + payload) — new, replaces the per-kind latches
  `ttfReflowJumpPending` / `pendingOffsetJump` as the funnel's input;
- `ttfReflowSeedPage` — unchanged (font-preview seed; consumed by the same
  funnel: a seed converts to a `CharOffset`-equivalent page target before
  evaluation, preserving the peek-don't-consume semantics it already has);
- `ttfCurrentCharStart`, `ttfRestoreLastPage` — unchanged;
- saved-position fields (`ttfSaved*`) — cleared by the funnel when consumed or
  unmappable, not by the chokepoint (they describe the RECORD, consumed once).

`ttfHasSavedPosition` remains the record-loaded latch; it is consumed only by
the funnel (§3.2) and by the spine-mismatch degrade — never by an
invalidation site.

## 5. Render-pass integration

### 5.1 Session-begin gate must open for a pending restore

The session-creation gate at `:3306` opens on
`needFullBuild || (resolved && target >= ttfPageCount)`. With a pending
unresolved restore (not covered yet), NEITHER holds — the first pass after
reopen would build nothing and paint the popup forever. The gate becomes:

```cpp
pendingRestore || needFullBuild || (resolved && target >= ttfPageCount)
```

### 5.2 One chunk per pass toward the target

The chunked pump at `:3331-3360` already builds one chunk per pass while
`target >= availablePageCount`. For a pending restore the loop condition uses
the cover check instead: keep pumping while `!covered && !terminal`, exactly
one `pumpChapterChunk(..., BUILD_PAGES_PER_CHUNK)` per pass, `requestUpdate()`
between passes (input polled, buttons alive — the standing soak requirement).
`needFullBuild` stays true ONLY for Percent/LastPage kinds, preserving their
wait-for-total semantics.

### 5.3 Worker/claim interplay unchanged

The `fibpResumeClaim` takeover (`:3290-3298`) keeps its current rule: the
reader takes the spine back when it needs pages the worker's partial lacks —
which for a restore is exactly when the cover check says "not covered". No new
worker protocol. The FIBP worker's own builds continue to commit partials the
next pass can resolve against.

### 5.4 Superseding navigation cancels the restore

A navigation press processed between restore passes clears the pending restore
and takes effect as a normal navigation (the anchor no longer describes where
the user wants to be — their explicit navigation wins). This follows
`reader-position-and-reindex.md` §"A superseding navigation must cancel a
pending one-shot": the cancel happens at the ENTRY of the superseding path, so
the fast/slow render branches cannot drift. A press during a restore pass is
never dropped (input is polled between passes; coalescing per the standing
queued-press requirement).

## 6. Decision log (coding-philosophy: DRY > SOLID > KISS)

- **DRY** — one resolver funnel replaces four mapping blocks that each
  duplicated the "can I map yet?" question; one popup painter replaces the
  Indexing call sites. The unified `PendingTarget` is the single source of
  "what is the reader waiting for".
- **SOLID** — the watermark-ambiguity rule (§3.3) is the fail-closed answer to
  a partial index: a clamped map is refused, never trusted, and pending state
  is never consumed before it is servable. The terminal clamp (§3.2 case 2)
  guarantees the state cannot pend forever on a complete index.
- **KISS** — no new persisted state, no record format change, no migration;
  generation remains a fast-path check rather than being threaded through a
  versioned anchor scheme. The rejected heavier alternative (storing
  byte-quantized progress positions per generation) would duplicate knowledge
  the layout engine already re-derives.
- **Lifetime** — `PendingTarget` is a POD struct copied by value; no pointers,
  no cross-call lifetime questions (coding-philosophy lifetime rule 1).

## 7. Risks

- **Prefix pages are stable across a generation bump.** Pages are built in
  order and a partial prefix is a true prefix of the final index ONLY under the
  same generation. ACROSS generations the anchor maps to whatever page the NEW
  layout assigns that char offset to — which is the feature (same text
  position under new layout), not a bug. The doc it supersedes flags the same
  assumption; it needs the device check it never got (§8.3).
- **`sessionBytesTotal()` may be an estimate** for some chapter inputs. The
  bar is cosmetic; a wobbling estimate does not affect correctness. Log the
  consumed/total once per pass at LOG_DBG for soak verification.
- **Anchor before the chapter's first text** (image-first chapters):
  `pageForChar` maps to page 0 — correct and terminal.
- **Anchor beyond a FINISHED chapter** (record written in a chapter that later
  shrinks after a settings change): terminal clamp to last page (§3.2 case 2).
  This is the only "position not exactly restored" case and it is bounded by
  the text itself.

## 8. Verification

### 8.1 Host unit tests (pure resolver policy, `ChapterIndexTarget` fake)

Mirroring `test/fibp_prefetch_policy/`:

1. target within built prefix → resolves, pending cleared;
2. target == charStart(last) on a partial → resolves to last page;
3. target past watermark on a partial → pending kept, no page served
   (peek-don't-consume: drive the resolver REPEATEDLY past the first chunk
   boundary — a single resolve never reaches the ambiguity);
4. build advances one chunk → next resolve succeeds at the target;
5. complete index, target beyond totalChars → clamps to last page, clears;
6. generation mismatch with saved anchor → CharOffset path (the §1.1
   regression, as a sequence test: save → bump generation → restore → assert
   resolved page, the "test of the SEQUENCE" rule);
7. spine mismatch → chapter start;
8. percent/last-page kinds → needFullBuild preserved.

### 8.2 Build gates

`pio run -e x4pro` first (standing default target), then one env from EACH
gating class of `CROSSPOINT_TTF_READER` per the build-flag matrix; host
`ctest` green; `./bin/clang-format-fix -g`.

### 8.3 Device (required, cannot be substituted)

Serial via `scripts/debugging_monitor.py`; X4 Pro soak:

1. Read ~40 pages into a long chapter, change font size → same text position,
   Indexing popup + bar visible during the build, bar ends below 100% when the
   position resolves;
2. Close the book, change font size in settings, reopen → same position (the
   §1.1 headline fix; today this lands at chapter start);
3. Same two checks with orientation change mid-chapter;
4. KOReader sync from a remote position mid-chapter with a cold cache →
   resolves without a full-index wait;
5. Press next/prev DURING a restore → navigation takes effect, restore
   cancelled, no stuck popup;
6. Log evidence: `ttfResolveTargetPage` resolves via the covered path rather
   than falling through to `needFullBuild`; input-side progress logged per pass.

## 9. Non-goals

- No `progress.bin` format change; no migration (16-byte record unchanged).
- TXT/XTC readers (separate follow-up).
- Legacy Section-path improvements from the superseded doc (mirror-clear guard
  on rebuild passes, proportional fallback from the pre-invalidation page) —
  re-scoped as a follow-up PR; the legacy path's block-to-target loop already
  restores correctly in the common case.
- Sub-page scroll restoration — page granularity is the ceiling (unchanged).
- Prefetch policy changes (#193 machinery reused as-is).

## 10. Amendments (review round, PR #203)

Qodo/Kody/CodeRabbit review findings and their resolutions:

1. **Page target kind added.** Page-anchored records (charOffset 0, e.g. KOReader
   remote-accept) and the preview seed are page numbers, not char offsets. The
   old funnel cast them to `CharOffset`, mapping page N as character N (page 0
   for every offset-0 record). `PendingTarget::Kind::Page` resolves directly
   once that many pages exist; terminal-clamps when the index is complete.
2. **Target origin recorded.** `PendingTarget.origin` (Reflow/Seed/Saved/
   OffsetJump/Anchor) replaces numeric-equality latch matching, which conflated
   a link resolving to the saved offset with the saved record itself.
3. **Derived targets are stored.** The funnel writes every latch-derived target
   into `pendingRestoreTarget_`, so the §5.1/§5.2/§3.5 gates open for
   saved-position/offset/anchor restores — previously only invalidation-seeded
   targets pumped the build; others served placeholder pages without building.
4. **evaluate() is pure.** Latch clearing moved to the caller, which owns the
   latches and reads the origin; the `charOffset == 0` clearing contract is gone
   (a nonzero saved anchor never cleared the latch before).
5. **§7 degrade at derivation.** A saved record bound to another spine or a
   stale generation degrades to the chapter start inside `deriveTarget()`. The
   old numeric check could never fire for the reopen-with-mismatch case
   (derivation required a generation match first) and left the latch set,
   shadowing later jumps.
6. **Seed consumed on any resolution.** `ttfReflowSeedPage` was never cleared,
   so every later pass re-derived the seed target and froze the position
   mirror. Any resolution now clears it (a resolution supersedes the hint).
7. **Stale charStart fallback removed.** The funnel no longer re-derives a
   target from `ttfCurrentCharStart` (the last rendered page's charStart), which
   snapped slow page turns back to the current page and shadowed jumps;
   `ttfInvalidateCaches()` already stores the reflow anchor explicitly.
8. **Single struct.** `EpubReaderActivity::PendingTarget` aliases
   `ttf_resolve::PendingTarget` (no two-struct `static_cast` drift), and all
   members are value-initialized (no garbage `kind` on first pass).
9. **Popup on every blocked pass; 64-bit percent.** `ttfShowIndexingPopup()`
   redraws on each pass that blocks painting (a restore from a substantial
   partial cache never took the session-start branch); the percent math is
   64-bit (consumed > ~42.9 MiB wrapped `consumed * 100`).
10. **coverCheck refuses page counts beyond the uint16 page-index domain**
    instead of truncating the `pageCharStart` cast.
