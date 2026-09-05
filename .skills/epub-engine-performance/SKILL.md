---
name: epub-engine-performance
description: "Optimisation discipline for lib/Epub/ on CrossPoint X4 Pro (ESP32-S3, 8MB PSRAM) and the C3 380KB cap. Use when sizing caches, choosing DRAM vs PSRAM, deciding whether to add a new in-RAM cache, or tuning the ZIP / CSS / image-decode / page-build pipeline. Covers: zipCache hot path, BookMetadataCache, the .bin / .partmeta / .html on-SD cache tier, when NOT to copy .release() into default_delete, when PSRAM is a regression, why no compression, and the project rule that extra SD writes for recoverable work are a regression."
---

# EPUB Engine Performance (CrossPoint)

This is the procedure for any change that touches the EPUB rendering
pipeline on CrossPoint. It is the operational companion to
`docs/design/2026-09-03-epub-engine-pipeline-and-io.md` (the v3 design
doc) and to the actual code that landed in PR #55.

## Two distinct boards, two distinct constraint sets

- **C3 (default env)** — single-core RISC-V @ 160 MHz, **~380 KB DRAM,
  no PSRAM**, one 48 KB framebuffer. Anything DRAM-first on the C3 is
  a regression; anything that doesn't gate on `BOARD_HAS_PSRAM` is a
  C3 regression.
- **X4 Pro (x4pro env)** — dual-core Xtensa LX7, 8 MB OPI PSRAM, same
  48 KB framebuffer (or 96 KB in `x4pro_perf` Phase 4). PSRAM is
  free, dual-core is real; the C3 constraints do not apply.

**X4C** is a PSRAM board that runs the same C3-style resource
protocol (small DRAM, no aggressive allocation). The `!defined(FREEINK_DEVICE_X4C)`
gate in `Epub.cpp:19-23` is the canonical pattern.

**The mistake to avoid:** optimising for X4 Pro's PSRAM and forgetting
to gate the change on `#ifdef BOARD_HAS_PSRAM`. Result: C3 allocates
the same way, OOMs, and the device crashes. **Every PSRAM placement
must be guarded.** The static_assert failure on D.1 (BuildContext) and
D.4 (JPEGDEC/PNG) was a different mistake — putting a typed
`std::unique_ptr<T, fn-deleter>` into PSRAM leaks the deleter to a
default `delete`. Both mistakes are easy to make.

## Project resource rules (the hardline)

1. **380 KB DRAM is the cap on C3 / x4c.** Every byte counts;
   fragmentation matters more than total usage. The `heap-discipline`
   skill in this same `.skills/` directory is the operational gate.
2. **No compression (zstd, LZ4, etc.).** The battery / CPU cost on C3
   exceeds the SD bytes saved. Decision recorded in
   `docs/design/2026-09-03-epub-engine-pipeline-and-io.md` §0.
3. **No `std::string` in hot paths** on the parser / cache paths.
   `std::string_view` for read-only access, `char[]` + `snprintf` for
   construction. Documented in the project's CLAUDE.md.
4. **XML context bytes capped at 1024** (expat). Don't bump.
5. **Single framebuffer** on the C3 path. The double-framebuffer flip
   in `GfxRenderer.cpp:120-131, ~340-370, ~2449-2504` is Phase 4
   (`x4pro_perf` env only).
6. **SD card is the storage tier.** No PSRAM replacement of SD.
7. **PSRAM-equivalent mmap reads are acceptable on S3** (different
   from DRAM-mmap; check the FreeInk SDK docs before assuming).
8. **Minimize SD writes.** Extra temp files for work that is
   recoverable on restart are a regression. The project explicitly
   rejected the named-partial snapshot for the active build
   checkpoint on these grounds (see §E.5 in the v3 design doc).
9. **`new` is not nothrow on ESP32.** Use
   `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h` (always).
10. **`#ifdef BOARD_HAS_PSRAM` is the canonical PSRAM gate** —
    not build env, not `BOARD_X4PRO` alone.

## Hot path: how a page reaches the framebuffer

1. User taps a spine item in the reader → `EpubReaderActivity`
   constructs `Section(spineIndex)`.
2. `Section::loadSectionFile()` reads
   `sections/<spineIndex>.bin` from SD. If the version mismatches or
   the parameters (font, line compression, etc.) differ, the file is
   rejected and a full rebuild starts.
3. On rebuild: `Section::startBuild()` streams the unzipped HTML from
   the EPUB to `html/<spineIndex>.html` on SD (NOT to PSRAM — HTML
   caches are durable, not transient), opens a tmp `.bin`, and
   instantiates `ChapterHtmlSlimParser` on the tmp.
4. `Section::buildSomeMore(0)` parses the HTML in a loop, calling
   `onPageComplete()` for each laid-out page. Each page is
   serialised to the tmp `.bin` and an entry is appended to
   `BuildContext::lut`. No active-loop checkpoint (see "Crash
   recovery" below).
5. `Section::finalizeBuild()` writes the header + LUT trailer,
   renames the tmp to `<spineIndex>.bin`. **This is the only time
   the durable partial is written.**
6. Page turns walk the in-RAM `pages` vector — no SD I/O. The
   `.bin` is re-read from SD only at chapter open.

## Caches, in order of access cost

| Cache | Storage | Allocator | Filled at | Lives until |
|---|---|---|---|---|
| `Epub::zipCache_` (CD scan) | PSRAM (8 MB on X4 Pro) | `heap_caps_malloc(MALLOC_CAP_SPIRAM)` + placement-new + function-pointer PSRAM deleter | `Epub::load()` (Eagerly today; should be lazy — see Copilot RLig) | `Epub` dtor |
| `BookMetadataCache` (spine + TOC + manifest) | DRAM (read-only after load) | `makeUniqueNoThrow<BookMetadataCache>()` | `Epub::load()` | `Epub` dtor |
| `BookMetadataCache::cssParser` | DRAM | `new CssParser(...)` | `Epub::load()` (only if `embeddedStyle`) | `Epub` dtor |
| `Section::build_` (active BuildContext) | **DRAM** (D.1 reverted — see PSRAM delter section below) | `makeUniqueNoThrow<BuildContext>()` | `Section::startBuild()` | `finalizeBuild` / `abandonBuild` / `suspendBuild` |
| `Section::pages` (in-RAM laid-out pages) | DRAM (after rehydration from `.bin`) | `std::unique_ptr<Page>` per page | `loadSectionFile()` or `buildSomeMore()` | `~Section()` (chapter close) |
| `Epub::html/<spineIndex>.html` (unzipped HTML) | **SD card** | `Storage` | `Section::startBuild()` once per spine item | `Epub::clearCache()` or book deletion |
| `Epub::sections/<spineIndex>.bin` (serialized Section) | **SD card** | `Storage` | `Section::commitBuildFile()` on build completion | `Epub::clearCache()` or settings-change invalidation |
| `Epub::book.bin` (book metadata cache) | **SD card** | `Storage` | `BookMetadataCache::buildBookBin()` during `Epub::load()` | `Epub::clearCache()` or settings change |
| `PixelCache::buffer` (image decode band) | PSRAM on X4 Pro | `heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM)` | `PixelCache::begin()` per image | `PixelCache::end()` (or dtor) |
| `HomeActivity::coverBuffer` (cover thumbnail) | PSRAM on X4 Pro | `heap_caps_malloc(needed, MALLOC_CAP_SPIRAM)` | `HomeActivity::storeCoverBuffer()` per tile | `HomeActivity::freeCoverBuffer()` (home screen exit) |
| `JPEGDEC` / `PNG` decoder scratch | **DRAM** (D.4 reverted — see PSRAM deleter section below) | `new (std::nothrow) T()` | Start of each `decodeToFramebuffer()` | End of decode |

**The pattern:** SD is the durable tier, DRAM is the active section,
PSRAM is the right place for transient buffers whose lifetime spans
the book reading session (ZipFileCache) or the home screen
(coverBuffer) or the image decode (PixelCache band). Don't put short
transient scratch in PSRAM if the cost of getting the deleter wrong
exceeds the benefit (D.4 lesson: ~20/44 KB transient, default `delete`
on PSRAM memory is a heap-corruption footgun).

## PSRAM delter trap: when `.release()` is wrong

The dangerous pattern:

```cpp
auto psram = makeUniqueNoThrowPsram<JPEGDEC>();   // unique_ptr<JPEGDEC, fn-ptr>
std::unique_ptr<JPEGDEC> jpeg(psram.release());    // LOSES the PSRAM deleter
// ... jpeg uses operator delete on PSRAM memory at end of scope ...
```

`std::unique_ptr<T>::reset()` and `~unique_ptr()` use the deleter
that the `unique_ptr` was constructed with. When you call
`.release()` you transfer ownership of the raw pointer; the new
`unique_ptr<JPEGDEC>` you build uses the **default deleter** which
calls `delete`. On ESP-IDF, `operator delete` routes to
`heap_caps_free` (works in practice), but it's not cap-safe in
general and is **not the project convention**.

**Two correct patterns:**

1. **Don't use a `unique_ptr` at all for PSRAM placement of a leaf
   type.** Use raw `heap_caps_malloc` + `heap_caps_free` and
   hand-write the destructor call. This is what
   `PixelCache::buffer` and `HomeActivity::coverBuffer` do today.
2. **If you must use `unique_ptr` with a custom deleter, store the
   deleter as a function pointer, not a lambda.** Function pointers
   keep the unique_ptr type uniform across boards and avoid the
   deleter-template-instantiation ambiguity that bit D.1. The
   `ZipFileCache` pattern (declared in `lib/Epub/Epub.h:121` and
   defined at `lib/Epub/Epub.cpp:1153`) is the canonical example.

**The reason D.1 and D.4 reverted:** `Section::build_` is a
`std::unique_ptr<BuildContext>` whose nested type `BuildContext`
contains `std::unique_ptr<ChapterHtmlSlimParser>`. The
`static_assert` on the deleter (in the C++ standard library's
`unique_ptr<...>`) requires the parser's full type at the
deleter-instantiation point. The parser is forward-declared in
`Section.h` (the existing convention); using a non-default PSRAM
deleter requires the parser's complete definition. The two patterns
incompatible. ~16 KB DRAM cost is accepted on X4 Pro. **Don't try
this migration again without restructuring the header first.**

## Crash recovery: no active-loop checkpoint

The codebase had a 5-second `commitBuildFile()` call in
`buildSomeMore()` (Phase 1, `b74d2a49`). It was reverted in
`f21b4a05` because `commitBuildFile()` closes + renames the open
tmp `.bin` mid-build, breaking the next `parseStep()`. The
replacement was a log-only stub that the project decided to also
remove in `95e4aa0f` because the log message misleads the
operator.

**The decision (recorded in §E.5 of the v3 design doc):** no
active-loop checkpoint. On a power loss mid-build, the next open
re-enters `loadSectionFile()` (returns false), then
`EpubReaderActivity` calls `Section::createSectionFile()` →
`startBuild()` → `buildSomeMore(0)`. The rebuild reuses the on-SD
HTML cache from Phase 2 (`html/<spineIndex>.html`, written once
at chapter open), so a full rebuild is ~1-2 s — much cheaper than
a first build.

**The reason no named-partial snapshot was added:** the project's
standing design goal is to *minimise SD writes*. A `partial-<N>`
snapshot every 5 s = ~720 extra SD writes per hour of reading for
work that is recoverable on restart. That's a wear regression for
recoverable work. **Don't propose an active-loop snapshot without
a stronger reason than "crash recovery" — restart-from-HTML-cache
is the recovery.**

## Caching decisions: when to add a new cache

Ask in order, stop at the first yes.

1. **Can the work be deferred to next book open?** If yes, no
   cache — just compute on demand. (The `RecentBooksStore` and
   `home-tile` covers use this pattern.)
2. **Can the work be skipped entirely on the next read?** If yes,
   no cache. (BookMetadataCache re-reads `book.bin` only on book
   open; the hot path skips it.)
3. **Is the work cheaper to re-do than the cache lookup?** If
   yes, no cache. (The CSS parser's `lowMemory` re-parse on
   C3 is faster than reading the cache file.)
4. **Is the work bounded and rarely-changing?** If yes, persist
   to SD. (`.bin`, `book.bin`, `html/*.html`.)
5. **Does the work fit in DRAM without fragmentation risk?** If
   yes, in-RAM on the `Epub` or `Section` owner. (zipCache map
   itself is 64-150 KB — fits in DRAM but kills C3, so it goes
   to PSRAM on X4 Pro and is gated off on C3 / x4c.)
6. **Otherwise, PSRAM on X4 Pro / x4c; no cache on C3.** The
   GFX render path doesn't need caches on C3 — the work is
   already cheap enough.

The ZipFileCache case is interesting: it qualifies for both
"in-RAM on the `Epub`" and "PSRAM on X4 Pro" — the answer is
both: the wrapper object is in PSRAM via placement-new, the map
itself uses the default allocator (DRAM for keys/buckets; see
Copilot RLi7 for the open issue). The next iteration will use a
PSRAM allocator on the map too.

## DR/WR hot spots (the data behind §B.7)

`docs/design/2026-09-03-epub-engine-pipeline-and-io.md` §B.7 has
the full table. The headline numbers (X4 Pro, typical 200-page
novel chapter, post-Phase-1):

| Operation | Pre-Phase-1 | Post-Phase-1 |
|---|---|---|
| `ZipFile::open()` (file-open syscalls) | 15-30 | unchanged |
| `loadAllFileStatSlims()` (CD scans) | 1 (since the library already calls it once at `Epub::load()`; the 15-30 figure was the ZipFile-construction count, not CD scans) | 1 |
| `getItemSize()` (per lookup) | ZipFile open + CD scan + 1 syscall | hashmap lookup |
| Page-turn SD reads | 0 (the `.bin` is in RAM after `loadSectionFile()`) | 0 |
| Cover thumbnail build | once per `HomeActivity::render()` | once per render, PSRAM-backed |
| CSS rule lookup | every time `parseCssFiles` walks the CSS file | `BookMetadataCache` cssParser with rules in DRAM |

**The `loadAllFileStatSlims()` count is 1, not 15-30.** The
verification report's pre-Phase-1 baseline was the ZipFile
construction count, which conflates the open syscall with the CD
scan. Fix the doc when measured numbers are available — Copilot
RLkW.

## The five invariant rules (the "no gotchas" list)

1. **Page turns NEVER touch SD.** The `.bin` is read once at
   chapter open. The in-RAM `pages` vector is the source of
   truth. If you find yourself adding SD I/O in a render loop,
   stop and redesign.
2. **The on-SD HTML cache is durable, not transient.** It is
   written once at chapter open and reused on rebuild. It is
   **not** the "transient PSRAM" cache the design doc v1
   originally proposed.
3. **The ZIP central directory is the dominant cost on book
   open.** Caching it in PSRAM (the ZipFileCache) cuts the
   file-open count from 15-30 to 1 on PSRAM boards, and
   dramatically reduces the per-lookup cost. Don't break the
   cache by re-scanning on every call.
4. **The decoder is transient.** JPEGDEC and PNG live for one
   image decode. Putting them in PSRAM is nice-to-have but the
   cost of getting the deleter wrong is heap corruption; the
   cost of leaving them in DRAM is ~20-44 KB transient, which
   is fine on X4 Pro.
5. **The profile counters are broken.** `g_load_all_stat_slims_count`
   and `g_fsfile_open_count` are declared in `lib/ZipFile/ZipFile.cpp`
   with internal linkage, never read or logged, and only
   `loadAllFileStatSlims()` (not `loadFileStatSlim()`) increments
   the CD counter. Until these are fixed, do NOT cite the
   per-book profiling numbers in design docs.

## Procedure: making a change to `lib/Epub/`

1. **Read the v3 design doc** (`docs/design/2026-09-03-epub-engine-pipeline-and-io.md`).
   The Phase 2 and Phase 3 status notes in §D.1, §D.4, §E.5 are
   the source of truth for what was tried, what worked, and what
   was reverted. Don't repeat the failed experiments.
2. **Check the resource constraints.** Is this DRAM-bound
   (C3 / x4c), PSRAM-OK (X4 Pro), or both? The gate is
   `#ifdef BOARD_HAS_PSRAM` for PSRAM placement; never
   `#ifdef BOARD_X4PRO` alone.
3. **Design doc update first.** If the change is non-trivial,
   update the design doc before writing code. The user (Nidoros)
   reviews the design doc before implementation lands.
4. **Build, test, push.** `pio run -e x4pro` first, then all
   other envs, then host unit tests. clang-format clean, cppcheck
   clean. PR description must explain the gate and the data flow.
5. **Review pass.** OpenCode `opencode/ling-3.0-flash-fin-free`
   for implementation; OpenCode `openrouter/poolside/laguna-s-2.1:free`
   for the second-opinion verification pass. CodeRabbit on the
   PR. `/answer-code-review` for batch thread resolution.
6. **If a review finds a real bug:** fix it in a follow-up
   commit, re-run CI, re-answer the thread. Don't batch real
   bugs with doc fixes.

## Pitfalls

- **`makeUniqueNoThrow` does not put things in PSRAM.** It uses
  `new (std::nothrow)` with the default allocator. To put
  something in PSRAM, you need `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
  + placement-new, and a matching `heap_caps_free` (or a
  function-pointer PSRAM deleter on a `unique_ptr`).
- **The `makeUniqueNoThrowPsram<T[]>(size)` helper in
  `lib/Memory/Memory.h:60-85` is dead code as of `95e4aa0f`.**
  The array overload has a deleter-type mismatch that Copilot
  flagged. Don't use it; use raw `heap_caps_malloc` + manual
  `free` instead. (Or fix the helper — but only if there is a
  real call site.)
- **Don't `git config --global` any identity** for commits in
  this repo. The identity is `Antoine Aflalo
  <197810+Belphemur@users.noreply.github.com>` set per-push
  via `git config user.name "Antoine Aflalo" && git config user.email "197810+Belphemur@users.noreply.github.com"`. No
  Hermes-Agent contamination.
- **The `X4C` board is a PSRAM board that runs the C3-style
  resource protocol.** The `!defined(FREEINK_DEVICE_X4C)` gate
  in `Epub.cpp:19-23` excludes it from PSRAM allocations. Don't
  remove this gate.
- **The `freeink-sdk` is the longer-term parser fork direction.**
  Not in scope for current performance work. Don't mix
  `lib/Epub/` and `freeink-sdk/libs/book/FreeInkBook/src/epub`
  in the same PR.
- **CrossPoint stats UI work is on the sister fork**
  `github.com/uxjulia/crossink` (cloned at
  `~/workspace/eink/crossink/`). Port-and-extend, not redesign.
  Co-author commits that touch ported crossink patterns as
  `Co-authored-by: Julia Nguyen <julia@uxj.io>`.

## Related skills

- `heap-discipline` — the C3 / x4c allocation rules. Use
  together with this skill on any PR that touches memory.
- `control-flow-clarity` — the in-line return-vs-scope
  convention. The `Section::startBuild()` early-return paths
  need the local-owner pattern (Copilot RLjQ follow-up).
- `scope-discipline` — the project's gate on PR scope. A
  doc-only PR with .cpp/.h edits (this PR's RLjt finding) is a
  scope-discipline violation.
- `hal-and-abstractions` — the HalStorage / HalFile patterns
  used by the SD-cache tier.
- `refactor-for-review` — the refactor pattern that produces
  reviewable diffs.
- `requesting-code-review` — the user's review-request style
  (cite file:line, no hype, evidence > assertion).
- `crosspoint-reader-dev` (in `~/.hermes/skills/`) — the
  top-level CrossPoint reader workflow (build, test, release,

## Phase 3 Profiling Outcome (2026-09-06)

**Dual-core parse OFFLOAD was DEFERRED** based on profiling data from
X4 Pro with "The Infinite and the Divine" EPUB. Key findings:

- Parse is ~24-27% of (parse + render) time on cold builds, 0% on
  cached chapters — below the 30% threshold for CPU-bound parse
- PSRAM pressure during parse: 0% (well below 70% contention threshold)
- SD I/O per chapter: < 500ms (well below threshold)
- `buildSomeMore_yield` durations: 55-522us (already well-yielded)

**The dominant cost is the render/display pipeline: ~1100-1200ms per
page, of which ~850-950ms is e-ink panel waveform time.** CPU-bound
work (render + grayscale passes + restore) is only ~150-200ms.

**Next optimization focus:**
1. Overlap e-ink refresh (dead time) with next-page pre-rendering on
   the other core (requires careful framebuffer management — single
   framebuffer constraint)
2. Cold-build overlap: move `createSectionFile()` blocking loop to
   core 0 during `EpubReaderActivity::renderBook()` cold-build path
   (lines 1428-1442), requires thread-safety audit of Section state

The `BOOK_PROFILE` instrumentation code remains in-repo for future
re-evaluation after render optimizations are deployed.
  design-doc-before-code).
