# Preserve the reading position across a reindex (font change / cache invalidation)

Status: design — not yet implemented
Date: 2026-10-04
Relates to: #193 (short-chapter prefetch), `2026-09-18-freetype-backend-as-built.md` §3.5

## Problem

Changing the font (or any settings change that invalidates the layout cache)
sends the reader back to the **first page of the current chapter**, even when
they were 40 pages in. The expectation is to hold the same *place in the text*,
falling back to the chapter start only when no mapping exists.

## What already exists

The machinery for position preservation is largely present. Two independent
mechanisms exist, one per render path.

### Legacy path (`Section`)

- `rememberCurrentContentOffset()` (`EpubReaderActivity.cpp:4334`) snapshots
  `cachedVisibleTextOffset` from the current page via
  `Section::getVisibleTextOffsetForPage()`.
- `cachedVisibleTextOffset` is restored through
  `Section::getPageForVisibleTextOffset()` at `EpubReaderActivity.cpp:2441`,
  and later by `applyDeferredReposition()` (`:4298`).
- The build loop waits for the offset rather than for a page count:
  `section->buildReachedVisibleTextOffset(*offsetJump)` (`:2379`).

### TTF path

- `ttfInvalidateCaches()` (`:2729`) sets `ttfReflowJumpPending` when
  `ttfCurrentCharStart > 0`, which preserves the anchor across the generation
  bump.
- `ttfResolveTargetPage()` (`:2756`) maps it back with
  `ttf_->pageForChar(currentSpine, ttfCurrentCharStart, &page)` (`:2838`).
- Progress is saved generation-tagged via `progressManager.saveTtf(...)`
  (`:2687`), carrying `ttfCurrentCharStart`.

So the *design* is right. The failures are in the guards around it.

## Root causes

### 1. The reflow mapping demands a COMPLETE chapter index (primary)

`canMapCompleteOffset()` (`:2767-2771`) returns false unless `haveTotal`, i.e.
unless either the active session is `sessionDone()` with a matching generation
or the on-disk cache is complete and idle:

```cpp
const bool haveTotal = sessionHasTotal || completeCache;
...
return sessionHasTotal ? ttf_->sessionTotalChars() > offset
                       : (completeCache && ttf_->cacheTotalChars() > offset);
```

A font change invalidates the cache, so on the pass that needs to resolve the
position `completeCache` is false and the session has just been reopened — not
done. Every call site therefore takes the "not yet mappable" branch:
`needFullBuild = true; return false;` (`:2844`, `:2852`), and the jump state
stays pending.

The chapter must then be indexed **end to end** before the reader can return to
where they were. `needFullBuild` forces a full build (`:3261`), the reader
waits, and when the build finally completes `haveTotal` becomes true — but by
then the offset may already have been consumed or reset along the way
(`ttfCurrentCharStart = 0` at `:3072` when `!ttfReflowJumpPending`, and the
`clearDeferredReposition()` calls that wipe the legacy mirrors). The observable
result is the chapter start.

**The guard is too strict for its purpose.** Its stated reason (`:2834-2836`) is
that "a partial prefix clamps beyond-watermark offsets and would lose the
position" — that is a real concern for an offset *past* the built prefix, but it
does not hold for an offset that falls *within* the pages already built. Those
can be mapped exactly, from the partial index, with no full build.

### 2. `needFullBuild` defeats the incremental yield from #193

`needFullBuild` is computed at `:2844`/`:2852` before any pages are built, so
the render pass cannot take the readable-partial path that #193 added: the
`MIN_PAGES_TO_CLEAR_POPUP` yield at `:3273` is keyed on
`shouldClearBuildPopup(ttfPageCount)`, but the surrounding
`(needFullBuild || (resolved && target >= ttfPageCount))` condition commits to a
synchronous build first. A reader 40 pages into a long chapter waits for the
whole chapter to be reindexed before seeing anything.

### 3. The legacy path has no `pendingPercentJump` equivalent, and its mirror is wiped on a cache hit

`clearDeferredReposition()` (`:4329`) resets both `cachedChapterTotalPageCount`
and `cachedVisibleTextOffset`. It is called from the offset-restore success
path (`:2443`) and from `jumpToPercent` (`:1480`). More importantly, on a
successful cache load the mirrors are cleared unconditionally:

```cpp
if (cacheLoaded) {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}
```
(`:2297-2300`)

That is correct for a *fresh* load, but it also fires on the pass that
rebuilds after a font change, discarding the offset captured by
`rememberCurrentContentOffset()` at `:1597` — the exact sequence a font change
performs (capture offset → `section.reset()` → rebuild → mirror cleared).

### 4. Proportional fallback is coarse and only fires on a page-count mismatch

`applyDeferredReposition()` (`:4312-4315`) maps proportionally:

```cpp
const float progress = static_cast<float>(section->currentPage) /
                       static_cast<float>(cachedChapterTotalPageCount);
newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
```

This runs only when `section->pageCount != cachedChapterTotalPageCount` and only
when the offset mapping already failed. On a font change the page count *does*
differ, so it is the last line of defence — but it is computed from
`section->currentPage`, which a partial build has already clamped to the last
built page. The result is a position derived from a clamped value, not from
where the reader actually was.

## Design

### Goal

Restore the reader to the same **text position**, in order of preference:

1. exact visible-text-offset match (legacy) / char-offset match (TTF);
2. anchor match (TOC/bookmark hash), where an anchor exists;
3. proportional page fallback using the **pre-invalidation** page number, not a
   clamped one;
4. chapter start.

Steps 1-3 must not require a complete chapter index.

### Change 1 — allow offset mapping against a partial index (TTF)

Split "the offset is beyond what has been built" from "the chapter is fully
indexed". Map the offset whenever the target page is *within the available
prefix*; only demand a full build when it is genuinely past the watermark.

Conceptually, replace `canMapCompleteOffset(offset)` in the `ttfReflowJumpPending`
branch with a two-part test:

- **mappable now** — `ttf_->pageForChar()` succeeds for the offset against the
  pages built so far → resolve to that page, keep serving, no full build.
- **past the watermark** — the offset is beyond the built prefix → `needFullBuild`
  as today, so the build continues until the offset is covered.

This preserves the original intent of the guard (never map an offset the partial
index cannot represent) while removing the requirement that the *whole* chapter
be indexed before the reader can resume. `pageForChar` must be consulted against
the live partial index, and its "not found" result is the signal that the offset
is out of range.

### Change 2 — let the #193 yield apply to a reflow

Once Change 1 resolves the target from the partial index, the render pass should
be able to take the readable-partial path. The condition at `:3261` must not let
`needFullBuild` force a synchronous build when the target is already resolvable
from the pages built so far. Concretely: if the target resolved and
`target < ttfPageCount`, do not request a full build — serve the page and let the
remaining pages drain in the background ticks, exactly as `MIN_PAGES_TO_CLEAR_POPUP`
already does.

### Change 3 — stop discarding the offset on the rebuild pass (legacy)

Do not clear `cachedVisibleTextOffset` when the cache loaded but the load is
itself the consequence of an invalidation we captured an offset for. The mirrors
should survive until the offset is either applied or genuinely unmappable.

The minimal shape: guard the reset at `:2297-2300` so it does not fire when
`cachedSpineIndex == currentSpineIndex` and a captured offset is pending — i.e.
when the load is a same-chapter reload, not a fresh entry.

### Change 4 — proportional fallback from the remembered page (both paths)

`applyDeferredReposition()` should compute the ratio from the page the reader was
on *before* the rebuild (`cachedChapterTotalPageCount` and the captured page
number), not from `section->currentPage`, which a partial build has clamped.
Store the pre-invalidation page alongside the offset at capture time and use it
as the numerator.

## Non-goals

- No change to the `Section` / FIBP binary formats. This is a positioning policy
  change; if the implementation needs a new persisted field, bump
  `SECTION_FILE_VERSION` per `docs/file-formats.md`.
- Not attempting sub-page-accurate scroll restoration — page granularity is the
  ceiling, and the reader is page-turn based.
- Not touching the prefetch policy from #193.

## Risks

- **Mapping into a partial index can pick a different page than the complete
  index would.** A partial prefix lays out identically to the full index for the
  pages it contains (pages are built in order), so this should hold, but it is
  the central assumption and needs a device check.
- **`pageForChar` on a partial index may return the last built page rather than
  failing** for an out-of-range offset. If so, Change 1 degrades to "resume at
  the watermark" — acceptable, but it must be detected and distinguished from a
  genuine miss, or the fallback ordering breaks.
- Change 3 widens the lifetime of `cachedVisibleTextOffset`; a stale offset
  surviving into a *different* chapter would misplace the reader. The
  same-chapter guard has to be airtight.

## Verification

1. Host unit tests for the mapping policy as a pure function, mirroring
   `test/fibp_prefetch_policy/`: offset within prefix, offset past prefix,
   anchor present, no mapping available.
2. `pio run -e x4pro` plus the gating envs; host `ctest` green.
3. **Device (required, cannot be substituted):**
   - read ~40 pages into a long chapter, change font → must stay at the same
     text position, not jump to the chapter start;
   - repeat with a short chapter (< 10 pages, the #193 path);
   - rotate orientation mid-chapter → must also preserve position;
   - verify a saved position survives close/reopen after a font change;
   - serial log via `scripts/debugging_monitor.py` confirming
     `ttfReflowJumpPending` resolves via `pageForChar` rather than falling
     through to `needFullBuild`.