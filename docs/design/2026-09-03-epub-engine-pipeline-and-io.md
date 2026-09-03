# EPUB Pipeline End-to-End + I/O Reduction + Dual-Core / PSRAM Plan

**Date:** 2026-09-03
**Branch:** `design/epub-engine-mem-sdcard` (from `origin/develop` SHA `57ca4438`)
**Origin:** `https://github.com/Belphemur/crosspoint-x-reader`
**Supersedes:** `docs/design/2026-09-03-epub-engine-x4pro-optimisation.md` (v1)
**Scope:** Full pipeline walk, SD I/O reduction, dual-core/PSRAM plan. Design doc only — no `.cpp`/`.h` edits, no commits, no push.

---

## 1. Goal + Non-goals

**Goal:** Make EPUB reading measurably faster on X4 Pro (ESP32-S3, 8MB OPI PSRAM, SSD1677 800×480) by (a) mapping the full EPUB pipeline from tap-to-pixel so every SD read and CPU cycle is visible, (b) cutting SD I/O — the dominant cost, and (c) better utilising the dual-core CPU and PSRAM. The section cache (`.crosspoint/epub_<hash>/sections/<spineIndex>.bin`, version 45) and the book metadata cache (`book.bin`, version 10) are the hot path; both are hit on every book open and every page turn.

**Non-goals (carried forward from v1):**
- **No compression of the section cache.** Drop zstd, LZ4, deflate, anything. Battery on C3 matters more than SD bytes saved; the section cache stays uncompressed.
- **No breaking C3/Sticky/X4C/Paper Mono.** Every change is gated behind `#ifdef BOARD_HAS_PSRAM` or `#ifdef BOARD_X4PRO`.
- **No new external dependencies** unless shipped with the project (today none are added).
- **No `.cpp`/`.h` edits** in this task (doc-only).
- **Honest diagnostics.** When a hypothesis is unproven, label it as estimate and ask the user for evidence.

---

## 2. End-to-End Pipeline Walk (Section A)

Every stage below: file:line citation, what is allocated, what is read/written on SD, what runs on which core, an honest wall-time estimate (labelled), and the bottleneck.

### A.1 Home screen / library list open

**Entry:** `ActivityManager::goHome()` → `replaceActivity(std::make_unique<HomeActivity>(...))` (`ActivityManager.cpp:319-334`).

- `HomeActivity::onEnter()` (`HomeActivity.cpp:137`) calls `loadRecentBooks()` (`HomeActivity.cpp:38`).
- `loadRecentBooks()` reads `RECENT_BOOKS.getBooks()` → `RecentBooksStore::loadFromFile()` (not yet examined line-by-line; JSON deserialisation of the recent-books file). **SD read:** one small JSON file (size of recent-books list, <1KB). **Core:** main loop task (core 0 on S3). **Estimate:** <50ms.
- `HomeActivity::render()` (`HomeActivity.cpp:307`) draws the UI; **no heavy I/O in the initial render**.
- `HomeActivity::loadRecentCovers()` (`HomeActivity.cpp:58`) runs lazily *after* the first render (guarded by `!recentsLoaded && !recentsLoading` at `HomeActivity.cpp:369`). For each recent book whose cover is missing, it constructs a temporary `Epub` object: `Epub epub(book.path, "/.crosspoint"); epub.load(false, true);` (`HomeActivity.cpp:71-72`). **This is the expensive part** — it triggers a full `Epub::load()` per cover-generating book (see §2.2), but skips CSS parsing (`true` = skipLoadingCss).

**Verdict:** The home screen itself is fast. The cost is deferred into `loadRecentCovers()`, which fires asynchronously after render. **Bottleneck:** repeated `Epub::load(false, true)` calls for each recent book that lacks a cached thumbnail.

### A.2 User taps a book

**Entry:** `HomeActivity::onSelectBook(path)` → `activityManager.goToReader(path)` (`ActivityManager.cpp:286-306`) → `ReaderActivity::create()` → `EpubReaderActivity::create()`.

- `EpubReaderActivity::onEnter()` (`EpubReaderActivity.cpp:203`) calls `ReaderActivity::onEnter()` (`ReaderActivity.cpp:49`).
- `ReaderActivity::onEnter()` calls `loadBook()` (`EpubReaderActivity.cpp:300-366`).
- `loadBook()` does: `auto loadedEpub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");` then `loadedEpub->load(true, SETTINGS.embeddedStyle == 0)` (`EpubReaderActivity.cpp:301,317`). **This is the single most expensive call in the tap-to-page flow.**
- If the cache is missing (`uncached`), it draws an "INDEXING" popup and takes the framebuffer loan (`EpubReaderActivity.cpp:307-318`).
- After `load()` succeeds, `section.reset()` (line 315) — no section is allocated yet.

**Core:** main loop task (core 0 on S3). **Estimate:** 2–8 seconds on first open (cold cache), 1–3 seconds on warm cache (cache hit). **Bottleneck:** `Epub::load()` — see §2.3.

### A.3 `Epub::Epub::open()` / `Epub::Epub::load()` — the spine touchpoint

**Entry:** `Epub::load(const bool buildIfMissing, const bool skipLoadingCss)` (`Epub.cpp:416-579`). This is the heart of the pipeline.

**Constructors called at entry:** `Epub(filepath, cachePath)` stores `filepath` and derives `cachePath = "/.crosspoint/epub_<hash>"` (from `Epub.h`).

**Step-by-step through `load()`:**

1. `bookMetadataCache.reset(new BookMetadataCache(cachePath))` (`Epub.cpp:420`).
2. `cssParser.reset(new CssParser(cachePath))` (`Epub.cpp:422`).
3. **Cache hit path (`bookMetadataCache->load()`):** `BookMetadataCache::load()` (`BookMetadataCache.cpp:460-499`) opens `book.bin` (`Storage.openFileForRead("BMC", cachePath + "/book.bin", bookFile)`), reads the header, deserialises metadata strings, and reads the cumulative-size LUT into `cumulativeSizes` (`cumulativeSizes.reserve(spineCount)` at line 489). **SD read:** one sequential read of `book.bin` (typically 2–16KB for a novel). **Core:** main loop task. **Estimate:** <10ms.
4. If cache miss or CSS rebuild needed, `Epub::load()` falls through to the build path (`Epub.cpp:482-577`):
   - `bookMetadataCache->beginWrite()` (`BookMetadataCache.cpp:68-74`).
   - **OPF pass:** `parseContentOpf(bookMetadata)` (`Epub.cpp:500`) — see §A.3.1 below.
   - **TOC pass:** `parseTocNavFile()` or `parseTocNcxFile()` (`Epub.cpp:521-530`) — see §A.3.2 below.
   - `bookMetadataCache->endWrite()` + `buildBookBin(filepath, bookMetadata)` (`Epub.cpp:544,551`) — writes `book.bin` sequentially via `BufferedFileWriter` / `BufferedFileReader` with 4KB buffers (`BookMetadataCache.cpp:190-192`, `BUILD_IO_BUFFER_SIZE = 4096`).
5. `cssParser->clear()` + (optional) `parseCssFiles()` (`Epub.cpp:562-568`). CSS parsing is a heavy I/O path (temp `.css` files on SD) but is gated by `skipLoadingCss`.
6. `bookMetadataCache.reset(new BookMetadataCache(cachePath))` + `bookMetadataCache->load()` — reload the cache (`Epub.cpp:571-575`).

**A.3.1 `parseContentOpf()` — content.opf parse** (`Epub.cpp:49-146`):
- `findContentOpfFile()` (`Epub.cpp:17-47`): opens `META-INF/container.xml` via `ZipFile(filepath)` (`Epub.cpp:219`) + `getItemSize(containerPath)` + `readItemContentsToStream(containerPath, containerParser, 512)` (`Epub.cpp:34`). **512-byte stream buffer.**
- `ContentOpfParser opfParser(...)` constructed (`Epub.cpp:66`).
- `readItemContentsToStream(contentOpfFilePath, opfParser, 1024)` (`Epub.cpp:73`). **1024-byte stream buffer.**
- `readItemContentsToBytes(opfParser.guideCoverPageHref, &coverPageSize, true)` (`Epub.cpp:90`) — this constructs **another** `ZipFile(filepath)` at `Epub.cpp:836` (`readItemContentsToBytes`) and does a full in-memory inflate to a `malloc`-ed buffer. **One more ZipFile construction + full inflate.**

**A.3.2 TOC parse** (`Epub.cpp:148-215`):
- `parseTocNcxFile()` (`Epub.cpp:148-179`) or `parseTocNavFile()` (`Epub.cpp:181-215`): each calls `getItemSize()` + `readItemContentsToStream(item, parser, 1024)` — **1024-byte stream buffer**, one `ZipFile(filepath)` construction per call.
- Output: `TocNcxParser` / `TocNavParser` populate `bookMetadataCache` TOC entries via `addTocEntry()` calls (in the parser `write()` methods). Output size: typically 2–20KB of TOC entries.

**ZipFile constructions per book open (all are `ZipFile(filepath)` where `filepath` is the EPUB path):**
- `Epub.cpp:219` — `discoverCssFilesFromZip()` enumerates CSS files.
- `Epub.cpp:836` — `readItemContentsToBytes()` for cover page (cover probe).
- `Epub.cpp:853` — `readItemContentsToStream()` for cover page stream.
- `Epub.cpp:280` — `ZipFile(filepath).enumerateFileEntries(...)` in `parseCssFiles()`.
- Plus the implicit `ZipFile` in every `readItemContentsToStream` / `getItemSize` / `readItemContentsToBytes` call (lines 34, 73, 172, 208, 853, 924) — each is `ZipFile(filepath).readFileToStream(...)` / `ZipFile(filepath).getInflatedFileSize(...)` (`Epub.cpp:845-854,872-875`). **Each constructs a fresh `ZipFile` and opens/closes the SD file.**

**Core:** main loop task (core 0 on S3). **Estimate:** 1–4 seconds (dominated by ZIP re-reads and CSS parsing). **Bottleneck:** repeated `ZipFile(filepath)` constructions + small stream buffers.

### A.4 Parser one-shots (ContainerParser, ContentOpfParser, TocNavParser, TocNcxParser)

- **ContainerParser** (`ContainerParser.h`): parses `META-INF/container.xml` → outputs `fullPath` (a single `std::string`). Input: ~1KB XML. Output: one path string (~50–100 bytes). Lives in DRAM on the parser's stack; dropped after `findContentOpfFile()` returns.
- **ContentOpfParser** (`ContentOpfParser.h`): parses `content.opf` → outputs `title`, `author`, `language`, `coverItemHref`, `textReferenceHref`, `cssFiles` (vector of strings), `tocNcxPath`, `tocNavPath`, `guideCoverPageHref`. Input: typically 5–30KB. Output: ~1–5KB of strings. Lives in DRAM; the strings are moved into `bookMetadata` (`Epub.cpp:80-143`).
- **TocNavParser** (`TocNavParser.h`) / **TocNcxParser**: parse the TOC → call `bookMetadataCache->addTocEntry(...)` for each entry. Output: the TOC entries written into `book.bin` (the cache file). The parser objects themselves are stack-allocated; dropped after `parseTocNavFile()` / `parseTocNcxFile()` returns.

**Verdict:** Parsers are lightweight. Their output is small and either moved into `bookMetadata` or written into `book.bin`. **Not a bottleneck.**

### A.5 `Section::loadContent()` / `Section::createSectionFile()` — per-section build

**Entry:** `EpubReaderActivity::renderBook()` → `section.reset(new Section(epub, currentSpineIndex, renderer))` (`EpubReaderActivity.cpp:1315`) → `section->loadSectionFile(renderSpec)` (`EpubReaderActivity.cpp:1318`) → if cache miss, `section->createSectionFile(renderSpec, popupFn)` (`EpubReaderActivity.cpp:1345`).

**`Section::createSectionFile()`** (`Section.cpp:247-256`) calls `startBuild()` then `buildSomeMore(0)`.

**`Section::startBuild()`** (`Section.cpp:258-444`):
1. `epub->getSpineItem(spineIndex).href` → local HTML path (`Section.cpp:277-280`).
2. **HTML cache check:** if `Storage.exists(htmlPath)` (the unzipped HTML), reuse it (`Section.cpp:294-349`). Otherwise:
   - `epub->readItemContentsToStream(localPath, tmpHtml, 8192)` (`Section.cpp:322`) — **8KB stream buffer** for the unzip. This is the *only* place in the pipeline that uses an 8KB buffer; it writes to a temp `.html` file on SD.
   - `Storage.rename(tmpHtmlPath, htmlPath)` promotes the unzipped HTML (`Section.cpp:344`).
3. `makeUniqueNoThrow<BuildContext>()` — allocates the `BuildContext` on the heap (`Section.cpp:358`). `BuildContext` contains `unique_ptr<ChapterHtmlSlimParser> parser`, `vector<PageLutEntry> lut`, strings, `CssParser*`.
4. `makeUniqueNoThrow<ChapterHtmlSlimParser>(...)` (`Section.cpp:414-424`) — constructs the Expat-based slim parser. `PARSE_BUFFER_SIZE = 1024` (`ChapterHtmlSlimParser.cpp:28`); the parser's `partWordBuffer[MAX_WORD_SIZE+1]` = 201B on the stack (`ChapterHtmlSlimParser.cpp:28`).
5. `build_->parser->beginParse()` + `build_->totalBytes = build_->parser->parseTotalBytes()` (`Section.cpp:437-443`).

**`Section::buildSomeMore()`** (`Section.cpp:446-471`): calls `build_->parser->parseStep()` in a loop, accumulating pages. Each page emits a `Page` object via the callback, which serialises into the `.bin` file via `onPageComplete()` (`Section.cpp:87-107`). Pages are written incrementally — the `.bin` file is kept open (`Section.h:21 HalFile file`) and pages append to it.

**Where does the raw HTML come from?** From the EPUB ZIP (inflated) → streamed to `html/<spineIndex>.html` on SD (the *unzipped HTML cache*). Subsequent opens reuse this file (`Section.cpp:294`), so the inflation happens only once per book (or after cache invalidation).

**Core:** main loop task (core 0 on S3). **Estimate:** 2–10 seconds for the first page of a large chapter (the popup shows "INDEXING" during this). **Bottleneck:** ZIP inflate → SD write for the HTML cache + Expat parse + page layout.

### A.6 Page layout

Page layout happens inside `ChapterHtmlSlimParser::parseStep()` as each chunk of HTML is parsed. The parser emits `Page` objects; each `Page` contains `vector<shared_ptr<PageElement>> elements` (`Page.h:79-83`). Layout is the Expat state machine stepping through HTML and laying out blocks/lines. **No separate `layoutPages()` call exists** — layout is interleaved with parsing in `buildSomeMore()`. The `Section::PageLutEntry` LUT (`Section.h:28-33`) records `fileOffset`, `paragraphIndex`, `listItemIndex`, `visibleTextOffset` per page. `commitBuildFile()` (`Section.cpp:543-625`) writes the LUTs + anchor map + paragraph LUT + li LUT + visible-offset LUT into the `.bin`, patches the header, and atomically renames the tmp file over `filePath`.

### A.7 Section `.bin` cache write

**Header:** `HEADER_SIZE` (`Section.cpp:68-71`) = 46 bytes (version + fontId + lineCompression + extraParagraphSpacing + paragraphAlignment + viewportWidth + viewportHeight + pageCount + hyphenationEnabled + embeddedStyle + imageRendering + focusReadingEnabled + 5×uint32_t). `SECTION_FILE_VERSION = 45` (`Section.cpp:50`).

**When is it written?**
- **Every chapter open (first open):** `createSectionFile()` → `finalizeBuild()` → `commitBuildFile(SECTION_FILE_VERSION, 0, 0)` (`Section.cpp:640`). The `.bin` is written to a tmp file then renamed.
- **Partial (suspended) build:** `suspendBuild()` → `commitBuildFile(SECTION_FILE_PARTIAL_VERSION, ...)` (`Section.cpp:671`). Written when the reader exits, sleeps, or navigates away mid-build.
- **On settings change:** `applyOrientation()` (`EpubReaderActivity.cpp:1124`) calls `section.reset()` → triggers a rebuild on next render. The `.bin` is re-written with the new `ReaderRenderSpec`.
- **On chapter close:** the destructor `~Section()` calls `suspendBuild()` (`Section.cpp:85`) — writes a partial if pages were built.

**Verdict:** The `.bin` is written on *every chapter close* (via `suspendBuild()` in the destructor) and again on a full build. **This is a write-path I/O cost that is often overlooked.** Each `.bin` write is a sequential write of the page data + LUTs.

### A.8 Render task — core pinning and parse context

**Entry:** `ActivityManager::begin()` (`ActivityManager.cpp:40-54`):
```cpp
xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
    8192, this, 1, &renderTaskHandle, renderTaskCore);
```
where `renderTaskCore = 1` on S3 (`configNUM_CORES > 1`, `ActivityManager.cpp:41-42`). Stack 8192 bytes, priority 1. Comment at `ActivityManager.cpp:51`: *"Keep long renders/cover decodes off CPU 0's idle watchdog when available."*

**Where is `Section::loadContent` / `Section::createSectionFile` invoked?**
- `EpubReaderActivity::renderBook()` (`EpubReaderActivity.cpp:1265`) is called from `ReaderActivity::render()` (`ReaderActivity.cpp:175,194`).
- `ReaderActivity::render()` is called from `ActivityManager::renderTaskLoop()` (`ActivityManager.cpp:61-84`) → `currentActivity->render(std::move(lock))` at `ActivityManager.cpp:72`.
- **Therefore: the section build (parse + layout) runs on the render task, which is pinned to core 1.** The main loop task (which handles input, activity navigation, and calls `loop()`) runs on core 0 on S3.

**This is the key finding:** today, **parse AND render both run on core 1** (the render task). Core 0 hosts the main loop but the heavy chapter-build work happens on core 1 inside `renderBook()` → `createSectionFile()` / `buildSomeMore()`. Core 0 is *not* doing the parse; it is mostly idle during chapter builds, blocked on nothing because the render task holds `renderingMutex` and the loop yields.

**Page-turn path** (`EpubReaderActivity::pageTurn()` at `EpubReaderActivity.cpp:1171`):
1. `section->currentPage++` or `--` (`EpubReaderActivity.cpp:1193,1207`).
2. `section->loadPage(section->currentPage)` (`Section.cpp:780-794`) → `loadPageAt()` (`Section.cpp:745-778`): opens `filePath` (the `.bin`), reads the page LUT offset, deserialises the `Page` via `Page::deserialize()`. **SD read:** one open + one sequential read of the `.bin` (typically 1–8KB per page). **Core:** render task (core 1). **Estimate:** <5ms for a cache hit.
3. `p->render(renderer, ...)` renders the page to the framebuffer.

**Verdict:** Page turns are cheap when the `.bin` cache is hot (single SD read + deserialize + render). **Bottleneck:** the `.bin` open/read + the `Page::deserialize` + render — but all on core 1, so core 0 is free during page turns.

### A.9 Summary of core assignment

| Stage | Core | Task context |
|---|---|---|
| Home screen, recent books, settings | 0 | Main loop task |
| `Epub::load()` (OPF, TOC, CSS) | 0 | Main loop task (but called from `loadBook()` which is called from `onEnter()` on main loop) |
| `Section::createSectionFile()` / `buildSomeMore()` | **1** | Render task (`renderTaskLoop`) |
| `Section::loadPage()` (page turn) | **1** | Render task |
| `renderBook()` → `p->render()` | **1** | Render task |
| `loop()` (input, navigation) | 0 | Main loop task |

**Correction to v1 hypothesis:** the parse does NOT run on core 0 today. It runs on core 1, inside the render task, *while the main loop on core 0 is mostly idle*. This means dual-core offload of parse would compete with render on core 1 — the gain is not trivially "move parse to core 0".

---

## 3. SD I/O Inventory (Section B)

For every SD access during a book open + page turn, with byte counts and ranking.

### B.1 ZIP central-directory re-reads

`ZipFile(filepath)` is constructed per call (`Epub.cpp:219,828-836,853-854`). Each `ZipFile` constructor opens the EPUB file on SD (`ZipFile.cpp:278-281`). Each `readItemContentsToStream` / `getItemSize` / `readItemContentsToBytes` call constructs a fresh `ZipFile`, which opens the file, and `loadZipDetails()` scans the EOCD (`ZipFile.cpp:223-275`, scanning the last 1KB of the file) and `loadFileStatSlim()` scans the central directory (`ZipFile.cpp:115-193`).

**Cost:** each `ZipFile` open + central-dir scan = 2 SD reads (EOCD + central directory) + one `open()` syscall. For a typical novel, `readItemContentsToStream` is called ~10–30 times per book open (container.xml, content.opf, TOC, CSS files, cover image, each section HTML). **That is 20–60 SD reads just for central-directory scans.**

### B.2 Per-section inflate reads

`readItemContentsToStream` (`Epub.cpp:845-854`) calls `ZipFile(filepath).readFileToStream(path, out, chunkSize)`. The chunk size is:
- 512 bytes for `container.xml` (`Epub.cpp:34`)
- 1024 bytes for `content.opf` (`Epub.cpp:73`), TOC (`Epub.cpp:172,208`), CSS (`Epub.cpp:353`)
- 1024 bytes for cover image (`Epub.cpp:665,754`)
- 8192 bytes for section HTML (`Section.cpp:322`) — the only large buffer

Inside `readFileToStream` (`ZipFile.cpp:449-572`), the inflate loop reads `chunkSize` bytes per iteration (`ctx.readBufSize = chunkSize`), inflates, and writes. For a 100KB compressed chapter HTML, a 1024-byte chunk means **~100 inflate iterations, each doing an SD read**. **A typical 50KB chapter inflate = ~50 SD read syscalls.**

### B.3 Section `.bin` cache reads/writes

- **Read (`Section::loadSectionFile`)** (`Section.cpp:142-225`): opens `filePath`, reads `HEADER_SIZE` (46 bytes) + LUTs. **SD read:** one open + ~0.5–4KB sequential read.
- **Write (`commitBuildFile`)** (`Section.cpp:543-625`): opens `binTmpPath()`, writes pages + LUTs + header, renames. **SD write:** full page data (typically 5–50KB per chapter) + LUTs.
- **Read on page turn (`Section::loadPageAt`)** (`Section.cpp:745-778`): opens `filePath`, seeks to LUT offset, reads the page. **SD read:** one open + ~1–8KB.

### B.4 Book metadata cache

`BookMetadataCache::load()` (`BookMetadataCache.cpp:460-499`): opens `book.bin`, reads header + metadata strings + cumulative-size LUT. **SD read:** one sequential read of `book.bin` (typically 2–16KB). Written during `buildBookBin()` (`BookMetadataCache.cpp:167-367`) via `BufferedFileWriter` / `BufferedFileReader` with 4KB buffers (`BookMetadataCache.cpp:190-192`).

### B.5 Cover thumbnail read/generation

`generateCoverBmp()` (`Epub.cpp:640-727`) / `generateThumbBmp()` (`Epub.cpp:732-826`): reads the cover image from the ZIP via `readItemContentsToStream(coverImageHref, coverJpg, 1024)` (`Epub.cpp:665,754`) — **1024-byte chunk buffer** — writes to a temp file, then decodes to BMP. The cover BMP is cached on SD (`getCoverBmpPath()` at `Epub.cpp:635-638`), so subsequent reads hit the cached BMP, not the ZIP.

### B.6 Recent books / settings reads

`RecentBooksStore::getDataFromBook()` (`RecentBooksStore.cpp:114-140`) constructs a temporary `Epub` and calls `epub.load(false, true)` — triggers the full `Epub::load()` path (see §2.2). This is called during `HomeActivity::loadRecentCovers()` (`HomeActivity.cpp:69-71`) and `HomeActivity::loadRecentBookStats()` (`HomeActivity.cpp:115-135`). **These happen only at home-screen open, not mid-page-turn.** Confirmed: `HomeActivity::render()` at `HomeActivity.cpp:368-372` loads covers/stats lazily after the first render.

### B.7 SD I/O table — book open (cold cache)

| Operation | File:line | Count per book-open | Bytes per count | Total bytes | SD reads |
|---|---|---|---|---|---|
| ZipFile open + central-dir scan | `Epub.cpp:219,828-836,853-854; ZipFile.cpp:278` | ~15–30 (one per `readItemContentsToStream`/`getItemSize`/`readItemContentsToBytes`) | ~2KB (EOCD + CD) | 30–60KB | 30–60 |
| content.opf inflate | `Epub.cpp:73` | 1 | ~5–30KB (uncompressed) | 5–30KB | 1 (but 5–30 stream reads) |
| TOC inflate | `Epub.cpp:172,208` | 1–2 | ~2–20KB | 2–40KB | 1–2 |
| CSS inflate + temp round-trip | `Epub.cpp:353` | 1–N (one per CSS file) | ~1–50KB each | 1–50KB | 1–N |
| Cover image inflate | `Epub.cpp:665,754` | 1–2 | ~50–300KB | 50–600KB | 1–2 |
| Section HTML inflate (per section built) | `Section.cpp:322` | 1 per new section | ~50–200KB | 50–200KB | 1 |
| `book.bin` read | `BookMetadataCache.cpp:461` | 1 | 2–16KB | 2–16KB | 1 |
| `book.bin` write (`buildBookBin`) | `BookMetadataCache.cpp:169,190` | 1 | ~4–32KB | 4–32KB | 1 write |
| Section `.bin` write (`commitBuildFile`) | `Section.cpp:543,640` | 1 per section built | 5–50KB | 5–50KB | 1 write |

**Top-3 I/O costs per book-open (cold cache):**
1. **Cover image inflate** (50–600KB, 1–2 SD reads) — the single largest transfer.
2. **Section HTML inflate** (50–200KB per section, 1 SD write for the `.bin` + 1 SD write for the `.html` cache) — the second largest, and it happens per section.
3. **ZIP central-directory re-reads** (30–60KB total across 15–30 `ZipFile` constructions, but each is a small 2KB scan — the *number* of SD transactions is the killer, not the bytes).

### B.8 SD I/O table — page turn (cache hot)

| Operation | File:line | Count per page-turn | Bytes per count | Total bytes | SD reads |
|---|---|---|---|---|---|
| Section `.bin` read (`loadPageAt`) | `Section.cpp:745-778` | 1 | 1–8KB (one page) | 1–8KB | 1 |
| Page render to framebuffer | `GfxRenderer.cpp` | 1 | — (DRAM) | — | 0 |
| (Optional) next-page prewarm | `EpubReaderActivity.cpp:483-493` | 0–1 | 1–8KB | 1–8KB | 0–1 |

**Top-3 I/O costs per page-turn (hot cache):**
1. **Section `.bin` read** (1–8KB, 1 SD read) — the dominant cost.
2. **Next-page prewarm** (optional, 1–8KB) — already overlaps with render via the idle prewarm at `EpubReaderActivity.cpp:472-493`.
3. **None significant** — page turns are I/O-light when the cache is hot.

**Verdict:** The I/O bottleneck is **book-open, not page-turn**. Page turns are fast when the `.bin` cache is present. The optimisation target is therefore (a) book-open I/O (ZIP re-reads, cover, section HTML inflate) and (b) write-path I/O (`.bin` writes on chapter close).

---

## 4. Dual-Core Utilisation Plan (Section C)

### C.1 Today's core assignment (confirmed)

- **Core 1:** render task (`ActivityManager.cpp:46-52`), stack 8192, priority 1. Runs `renderTaskLoop()` → `currentActivity->render()` → `renderBook()` → `Section::createSectionFile()` / `buildSomeMore()` → `ChapterHtmlSlimParser::parseStep()`. **Parse and render share core 1.**
- **Core 0:** main loop task (`ActivityManager::loop()`). Handles input, activity navigation, home screen. During a chapter build, the main loop is *not* blocked — it continues calling `loop()`, but the render task holds `renderingMutex` and the loop's `requestUpdate()` notifications are queued. Core 0 is mostly idle during builds.
- **Proof:** `renderBook()` at `EpubReaderActivity.cpp:1265` is called from `ReaderActivity::render()` (`ReaderActivity.cpp:194`) which is called from `ActivityManager::renderTaskLoop()` at `ActivityManager.cpp:72`. The render task is pinned to core 1 (`ActivityManager.cpp:41-42`).

### C.2 Options analysis

**Option A — Parse on core 0, render on core 1, fully decoupled (NEW):**
- Create a dedicated parse task pinned to core 0, with its own stack (4096–8192 bytes). Feed it a queue of "next section to load" from the main loop.
- Render task on core 1 reads already-parsed sections from PSRAM.
- Data flow: main loop → `xQueueSendToBack(parseQueue, &spineIndex)` → parse task → `Section::createSectionFile()` → parsed `.bin` written to PSRAM → render task reads from PSRAM.
- Expected wall-time delta: **significant** — book-open could overlap ZIP inflate (core 0) with render of previous chapter (core 1). Page turns could pre-fetch the next section on core 0 while core 1 renders the current page.
- Complexity: **high** — requires a thread-safe queue, PSRAM-resident section ring buffer, mutex around the `.bin` file, and careful lifecycle management (parse task must be paused/resumed with activity transitions).
- Risk: medium — FreeRTOS task priority inversion, PSRAM contention if both tasks allocate from PSRAM simultaneously, watchdog on core 0 if parse starves.

**Option B — Parse on core 0 in the loop task, render on core 1 (today's effective behavior):**
- Today, the parse runs on core 1 (inside the render task). Moving it to core 0's loop task would mean: main loop calls `section->createSectionFile()` which blocks the loop until the build finishes (or until `buildSomeMore()` yields).
- **This is the current behavior with parse moved off core 1.** The loop would block during `createSectionFile()` / `buildSomeMore()` — the UI would freeze until the first page is laid out.
- Expected wall-time delta: **negative** — the loop blocking means no input handling, no rendering of the previous page, no popup updates during the build.
- Complexity: low (just change which task calls `renderBook()`).
- Risk: high — UI freezes during chapter build; unacceptable UX.

**Option C — Parse on core 1, render deferred to a third task (REJECTED):**
- Core 1 already hosts render; adding parse would compete for the same core. No gain.
- **Rejected.**

### C.3 Recommendation: Option A (dedicated parse task on core 0)

The single most impactful change is to move the chapter build off the render task and onto a dedicated parse task on core 0. This:
1. Frees the render task (core 1) to render the *current* page while the *next* page is being parsed on core 0.
2. Keeps core 0's main loop responsive (the parse task, not the loop, does the heavy work).
3. Enables page-turn pre-fetch: when the user is on page N, the parse task builds page N+1 in the background.

**Data flow for Option A:**
- A `ParseQueue` (FreeRTOS `QueueHandle_t`) of `SpineIndex` values, created in `ActivityManager::begin()`.
- Parse task (`xTaskCreatePinnedToCore(&parseTaskTrampoline, "EpubParse", 8192, this, 1, &parseTaskHandle, 0)`) — pinned to core 0, same priority as render task.
- When `EpubReaderActivity::renderBook()` needs a section, it sends the `spineIndex` to the queue and waits (with a timeout) for the parse to complete.
- The parse task calls `section->createSectionFile()` and writes the `.bin` to PSRAM-resident storage.
- The render task reads the `.bin` from PSRAM and renders.

**Honest caveat:** The main parse path today is *already* inside the render task on core 1. Moving it to a dedicated core-0 task means the render task no longer does the build — it only renders. This is a real architectural change and should be profiled before committing. The user should verify with `core_id()` logging at each pipeline stage.

### C.4 S3 hardware JPEG decoder probe

**Finding:** `esp_jpeg_dec.h` exists in the Espressif component cache at `~/.cache/Espressif/ComponentManager/service_d92d8f1e/espressif__esp_new_jpeg_1.0.2_e1e7e699/include/esp_jpeg_dec.h`. This is the ESP-IDF `esp_new_jpeg` component, which exposes `esp_jpeg_dec.h` with `jpeg_dec_handle_t`, `esp_jpeg_decode()`, and block-decoder mode support.

**However:** the project does **not** use `esp_jpeg_dec`. It uses `JPEGDEC` (`<JPEGDEC.h>`) from the FreeInk SDK — a C++ wrapper that calls `jpegOpen`/`jpegClose`/`jpegRead`/`jpegSeek` callbacks (`JpegToFramebufferConverter.cpp:55-92`). The `JPEGDEC` class is heap-allocated (~20KB) and does its own inflate + scaling. The `esp_jpeg_dec` API is C-based and would require a different integration path.

**Conclusion:** The hardware JPEG decoder is available in the toolchain but not wired up. Integrating `esp_jpeg_dec` would require replacing the `JPEGDEC` wrapper and the callback-based I/O model. This is a Phase 4 change and is gated behind profiling — the `JPEGDEC` path works today and the `esp_jpeg_dec` integration is non-trivial. **Not recommended for Phase 1.**

---

## 5. PSRAM Utilisation Plan (Section D)

For each bulky structure, DRAM → PSRAM migration plan with gate and risk. `HalDisplay::BUFFER_SIZE = 48,000 bytes` (`HalDisplay.h:32`). Framebuffer stays in DRAM (display controller accesses DRAM directly).

### D.1 Parsed section cache (the `.bin` payload materialised in RAM)

Today: during `Section::createSectionFile()` / `buildSomeMore()`, each `Page` object is built in DRAM and serialised to the `.bin` file. The `BuildContext::lut` (`vector<PageLutEntry>`, `Section.h:39`) and the `ChapterHtmlSlimParser` working set are in DRAM. After `finalizeBuild()`, only the `.bin` file on SD remains; the DRAM is freed.

**Migration:** On X4 Pro, keep the active section's `Page` objects and LUT in PSRAM during the build. The `BuildContext` can be allocated with `makeUniqueNoThrow<BuildContext>()` which, on PSRAM boards, routes `new` to PSRAM (ESP32-S3 heap allocation order: PSRAM first when `BOARD_HAS_PSRAM`). The `lut` vector and `Page` elements stay in PSRAM.

- **DRAM saved:** ~16KB (active LUT + parser working set per chapter).
- **PSRAM used:** ~1MB (cached HTML for the active section; typical novel chapter is 50–200KB HTML, well within PSRAM for multiple sections).
- **Gate:** `#ifdef BOARD_HAS_PSRAM`.
- **Risk:** Low. The `Page` objects are serialised to `.bin` on `onPageComplete()` anyway; keeping them in PSRAM during the build is an optimisation, not a correctness change.
- **Verification:** `LOG_INF` PSRAM at section open; re-open same chapter and measure time.

### D.2 Cover thumbnail buffer (`HomeActivity::storeCoverBuffer`)

`HomeActivity::storeCoverBuffer()` (`HomeActivity.cpp:159-179`) does `coverBuffer = static_cast<uint8_t*>(malloc(needed))` — allocates a cover tile buffer (~16KB in Portrait) on DRAM heap. `freeCoverBuffer()` frees it on exit.

**Migration:** On X4 Pro, redirect to PSRAM. Use `makeUniqueNoThrow<uint8_t[]>(needed)` which routes to PSRAM on PSRAM boards. The cover tile is only needed during `render()` — it can be a PSRAM-allocated unique_ptr stored in `HomeActivity`.

- **DRAM saved:** ~16KB (cover tile buffer).
- **PSRAM used:** ~16KB.
- **Gate:** `#ifdef BOARD_HAS_PSRAM`.
- **Risk:** Low. Cover tile is a transient buffer; PSRAM placement is safe.
- **Verification:** `LOG_INF` heap/PSRAM before/after `storeCoverBuffer()`.

### D.3 Decoded image band (`PixelCache::buffer`)

`PixelCache::buffer` is allocated with `malloc(bufSize)` at `PixelCache.h:91` where `bufSize = (bandRows + 1) * bytesPerRow`, capped at `MAX_BAND_BYTES = 24KB` (`PixelCache.h:59,90-91`). The band buffer is a streaming cache writer; it holds one MCU row band at a time.

**Migration:** Already PSRAM-friendly via `makeUniqueNoThrow<uint8_t[]>`. The `malloc` at `PixelCache.h:91` should be replaced with `makeUniqueNoThrow<uint8_t[]>(bufSize)` — on PSRAM boards, this routes to PSRAM automatically. The `JpegToFramebufferConverter.cpp:96-97` and `PngToFramebufferConverter.cpp:86-87,352` decoder allocations also heap-allocate (`new (std::nothrow) JPEGDEC()` / `new (std::nothrow) PNG()`), which on PSRAM boards will land in PSRAM.

- **DRAM saved:** ≤24KB (band buffer) + ≤20KB (JPEGDEC) + ≤44KB (PNG) = ≤88KB.
- **PSRAM used:** ≤88KB.
- **Gate:** `#ifdef BOARD_HAS_PSRAM` — but the `new (std::nothrow)` path already routes to PSRAM on PSRAM boards, so this may be automatic. Confirm with `ESP.getFreeHeap()` / `ESP.getFreePsram()` before/after decode.
- **Risk:** Low-Medium. Must confirm ESP32-S3 heap allocation order (PSRAM-first vs DRAM-first).
- **Verification:** `LOG_INF` PSRAM before/after decode.

### D.4 JPEG/PNG decoder state

`JpegToFramebufferConverter.cpp:96-97` (`constexpr size_t JPEG_DECODER_APPROX_SIZE = 20 * 1024`) and `PngToFramebufferConverter.cpp:86-87,352` (`constexpr size_t PNG_DECODER_APPROX_SIZE = 44 * 1024`). Both use `new (std::nothrow) JPEGDEC()` / `new (std::nothrow) PNG()` inside `decodeToFramebuffer()` / `getDimensionsStatic()`.

**Migration:** The `JPEGDEC` / `PNG` objects are heap-allocated and freed at the end of the decode function. On PSRAM boards, `new (std::nothrow)` routes to PSRAM. No code change needed for the allocation path — but add `LOG_INF` to confirm the placement. If confirmation shows DRAM allocation, wrap in `#ifdef BOARD_HAS_PSRAM` and use a PSRAM-pinned allocator.

- **DRAM saved:** ≤64KB (JPEG 20KB + PNG 44KB).
- **PSRAM used:** ≤64KB.
- **Gate:** `#ifdef BOARD_HAS_PSRAM`.
- **Risk:** Low. Decoder lifetime is short (single decode); PSRAM placement is safe.
- **Verification:** `LOG_INF` PSRAM before/after decode.

### D.5 ZIP central directory cache (carried from v1 §1)

Cache the parsed central directory in PSRAM at book open. `ZipFile(filepath)` is constructed per call (`Epub.cpp:219,828-836,853-854`); each construction opens the EPUB file and scans the central directory (`ZipFile.cpp:223-275,115-193`).

**Migration:** Add a PSRAM-resident `ZipFileCache` that parses the central directory once at `Epub::load()` time and caches `FileStatSlim` entries in a `unordered_map<string, FileStatSlim>` in PSRAM. Subsequent `getItemSize()` / `readItemContentsToStream()` calls hit the cache instead of re-scanning the ZIP.

- **DRAM saved:** — (no new DRAM allocation; cache lives in PSRAM).
- **PSRAM used:** ≤64KB (cached central directory + file offset map).
- **Gate:** `#ifdef BOARD_HAS_PSRAM`.
- **Risk:** Low. Central directory format is well-defined; a parsed cache is a read-only lookup table.
- **Verification:** `LOG_INF` PSRAM at book open; compare `ZipFile` open count before/after.

### D.6 Section pre-fetch buffer (NEW — for dual-core parse task)

If Option A (dedicated parse task on core 0) is adopted, the parsed result must live somewhere while the render task on core 1 consumes it. A PSRAM ring of 2–3 pre-built sections provides this buffer.

**Migration:** A `std::deque<SectionCache>` in PSRAM, where each `SectionCache` holds the `.bin` page data + LUT for one section. The parse task builds section N+1 into PSRAM; the render task reads section N from PSRAM. When the render task finishes section N, it signals the parse task to build N+1 (or N+2 if the user is fast).

- **DRAM saved:** — (buffer lives in PSRAM; the `BuildContext` on core 0 is smaller because the result is written directly to PSRAM).
- **PSRAM used:** ≤500KB (2–3 sections × ~50–200KB each, but only the LUT + active page data, not the full HTML).
- **Gate:** `#ifdef BOARD_HAS_PSRAM && BOARD_X4PRO` (only when dual-core parse is enabled).
- **Risk:** Medium. Requires the dual-core parse task (Option A) to be implemented first. Ring-buffer semantics (which section to evict) need careful design.
- **Verification:** `LOG_INF` PSRAM at section pre-fetch; measure page-turn latency with/without pre-fetch.

### D.7 PSRAM summary table

| Structure | DRAM saved (KB) | PSRAM used (KB) | Gate | Risk |
|---|---|---|---|---|
| Parsed section cache (active build) | ~16 | ~1000 | `BOARD_HAS_PSRAM` | Low |
| Cover thumbnail buffer | ~16 | ~16 | `BOARD_HAS_PSRAM` | Low |
| PixelCache band buffer | ≤24 | ≤24 | `BOARD_HAS_PSRAM` (auto via `new`) | Low-Medium |
| JPEGDEC decoder | ≤20 | ≤20 | `BOARD_HAS_PSRAM` (auto via `new`) | Low |
| PNG decoder | ≤44 | ≤44 | `BOARD_HAS_PSRAM` (auto via `new`) | Low |
| ZIP central-dir cache | — | ≤64 | `BOARD_HAS_PSRAM` | Low |
| Section pre-fetch ring | — | ≤500 | `BOARD_HAS_PSRAM && BOARD_X4PRO` | Medium |

---

## 6. SD I/O Reductions — Concrete Changes (Section E)

### Change E.1 — Larger stream buffers for `readItemContentsToStream`

**Today:** 512-byte buffer for `container.xml` (`Epub.cpp:34`), 1024-byte for `content.opf` (`Epub.cpp:73`), TOC (`Epub.cpp:172,208`), CSS (`Epub.cpp:353`), cover image (`Epub.cpp:665,754`). Only section HTML uses 8192 (`Section.cpp:322`).

**Proposal:** Bump all `readItemContentsToStream` calls to 4096 or 8192 bytes. The `ZipFile::readFileToStream` (`ZipFile.cpp:449-572`) accepts `chunkSize` as a parameter; pass 4096 or 8192. For the 1024-byte calls, this reduces the number of `inflate.readAtMost` iterations by 4–8×.

**Quantification:** For a 50KB chapter HTML inflate at 1024-byte chunks = ~50 SD read syscalls. At 4096-byte chunks = ~13 SD read syscalls. At 8192-byte chunks = ~7 SD read syscalls. **~7–8× syscall reduction.**

**Call sites to change:** `Epub.cpp:34,73,172,208,353,665,754` — add a `const size_t streamChunkSize = 8192` constant and pass it to every `readItemContentsToStream` call.

**Risk:** Low. The chunk size is a parameter, not a protocol change. Larger buffers use more DRAM transiently (~8KB per call), but these are short-lived and freed on return.

**Verification:** `LOG_INF` at each `readItemContentsToStream` entry; measure book-open time.

### Change E.2 — ZIP central-directory cache (carried from v1 §1)

**Today:** `ZipFile(filepath)` constructed per call (`Epub.cpp:219,828-836,853-854`). Each construction opens the EPUB file and scans the central directory.

**Proposal:** Cache the central directory in PSRAM at `Epub::load()` time. `loadAllFileStatSlims()` (`ZipFile.cpp:64-113`) already builds a `fileStatSlimCache` — but it is built per-`ZipFile` instance and discarded. Make it a PSRAM-resident singleton that persists for the book session.

**Risk:** Low. `loadAllFileStatSlims()` is already the right primitive; it just needs to be cached across `ZipFile` instances.

**Verification:** Count `ZipFile::open()` calls before/after; should drop from ~15–30 to ~1–2 per book open.

### Change E.3 — Sequential read-ahead for chapter inflate

**Today:** When `Section::startBuild()` unzips HTML (`Section.cpp:322`), it streams the HTML to `html/<spineIndex>.html` on SD. There is no prefetch of the next chapter.

**Proposal:** When the user opens chapter N, the engine knows the next spine item (`epub->getSpineItem(spineIndex + 1).href`). In the idle prewarm window (`EpubReaderActivity.cpp:472-493`), start inflating the next chapter's HTML into the PSRAM pre-fetch buffer (Section D.6) while the current chapter renders.

**Where to plug in:** `EpubReaderActivity::loop()` at `EpubReaderActivity.cpp:472-493` — the idle prewarm already checks `idlePrewarmSpine != currentSpineIndex`. Extend it to kick off `readItemContentsToStream` for the next chapter into the PSRAM buffer.

**Risk:** Medium. Requires the PSRAM pre-fetch buffer (Section D.6) and careful lifetime management (the pre-fetch must be cancelled if the user navigates away).

**Verification:** `LOG_INF` at pre-fetch start/complete; measure page-turn latency with/without pre-fetch.

### Change E.4 — Cover-once-or-on-import

**Today:** `generateCoverBmp()` (`Epub.cpp:640-727`) and `generateThumbBmp()` (`Epub.cpp:732-826`) check `Storage.exists(getCoverBmpPath())` first (`Epub.cpp:642,734`). If the BMP exists, they return `true` immediately. If not, they inflate the cover from the ZIP and write the BMP. **So the cover is generated once and cached on SD.**

**Verdict:** Cover generation is already cached-on-import. No change needed. The only SD cost is the `Storage.exists()` check on each home-screen open — negligible.

### Change E.5 — Section `.bin` write debouncing

**Today:** `~Section()` calls `suspendBuild()` (`Section.cpp:85`) → `commitBuildFile(SECTION_FILE_PARTIAL_VERSION, ...)` (`Section.cpp:671`) if pages were built. This writes the `.bin` on *every chapter close* (when the user navigates away or the reader exits).

**Proposal:** Debounce the partial write. If the user is actively reading (not navigating away), delay the `.bin` write until: (a) the user navigates to a different chapter, (b) the reader exits, or (c) a configurable timeout (e.g., 5 seconds) elapses since the last page turn. The partial file is still written on `suspendBuild()` in the destructor (which is always called), but the *content* can be the same as the last full build if no new pages were laid out since the last commit.

**Where to cite:** `Section::suspendBuild()` (`Section.cpp:658-695`), `Section::~Section()` (`Section.cpp:85`), `Section::commitBuildFile()` (`Section.cpp:543-625`).

**Risk:** Medium. If the device crashes before the debounced write, the last partial is lost — but the previous full build is still intact, so recovery is graceful (the old partial is shown until the new one is built).

**Verification:** Count `.bin` writes per session with/without debouncing; confirm no data loss on simulated crash.

### Change E.6 — No-write metadata fast path

**Today:** `BookMetadataCache::load()` reads `book.bin` (`BookMetadataCache.cpp:461`). The cache is re-written only when settings change (which triggers a rebuild). On a normal page turn, `book.bin` is read but not written.

**Proposal:** Confirm that `book.bin` is read-only during normal reading. The only write path is `buildBookBin()` (`BookMetadataCache.cpp:167-367`) during `Epub::load()` (cache build) or when CSS is rebuilt (`Epub.cpp:444-451`). Add a fast-path flag: if `book.bin` was loaded successfully and no settings changed, skip any write. This is likely already the case — verify by tracing all write paths.

**Where to cite:** `BookMetadataCache::buildBookBin()` (`BookMetadataCache.cpp:167`), `Epub::load()` (`Epub.cpp:482-577`).

**Risk:** Low. The write path is already limited to cache-build time.

**Verification:** Trace all `bookFile.close()` / `Storage.openFileForWrite("BMC", cachePath + bookBinFile, bookFile)` calls; confirm they only happen during `buildBookBin()`.

### Change E.7 — Concurrent read of EPUB + section cache

**Today:** Serial. The `Epub::load()` path (OPF + TOC + CSS) completes before `Section::createSectionFile()` is called (`EpubReaderActivity.cpp:317` → `renderBook()` → `createSectionFile()`). During `createSectionFile()`, the ZIP is read for the HTML; `book.bin` is not read concurrently.

**Proposal:** With the dual-core parse task (Option A), the ZIP read for the next section can overlap with the render of the current section. The parse task reads the ZIP; the render task reads the `.bin`. With the ZIP central-dir cache (Change E.2), the ZIP reads are minimised.

**Risk:** Medium. Requires the dual-core parse task.

---

## 7. Hardware-Aware Render Path (Section F)

### F.1 Single-buffer mode lift for X4 Pro only

**Today:** `EINK_DISPLAY_SINGLE_BUFFER_MODE=1` is set in `base:29` (`platformio.ini:29`) for ALL boards. This means there is only one 48KB framebuffer (`HalDisplay::BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT`, `HalDisplay.h:32` = 800×480÷8 = 48,000 bytes).

The `storeBwBuffer()` / `restoreBwBuffer()` path (`GfxRenderer.cpp:2449-2504`) copies the framebuffer in 8KB chunks (`BW_BUFFER_CHUNK_SIZE = 8000`, `GfxRenderer.h:42`) to allow for non-contiguous memory. `storeBwBuffer()` allocates `bwBufferChunks` (`frameBufferSize + BW_BUFFER_CHUNK_SIZE - 1) / BW_BUFFER_CHUNK_SIZE` = 7 chunks of 8KB each = 56KB total via `malloc` (`GfxRenderer.cpp:2461`). `restoreBwBuffer()` copies them back.

**The double-buffer flip path:** When `EINK_DISPLAY_SINGLE_BUFFER_MODE=0` (not currently set for any board), `GfxRenderer` would allocate a second framebuffer in PSRAM and flip between them. The `GfxRenderer::begin()` (`GfxRenderer.cpp:120-131`) calls `display.getFrameBuffer()` — with single-buffer mode, this returns one buffer. Without it, a second buffer would be allocated. The `storeBwBuffer()`/`restoreBwBuffer()` overhead (56KB of 8KB `malloc` chunks) would be eliminated by a double buffer — the grayscale plane is rendered directly to the second buffer, then swapped.

**Honest trace:** I have traced `GfxRenderer.cpp:120-131` (single-buffer begin), `2449-2504` (storeBwBuffer/restoreBwBuffer), and the `displayBuffer()` call at `GfxRenderer.cpp:1710-1714`. The double-buffer flip logic itself (the `#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE` branch in `GfxRenderer`) was not fully traced in this review — **flagged as an open question**. The v1 doc's §5 open question #2 stands.

**Proposal for X4 Pro:** Add a new env `[env:x4pro_perf]` (`platformio.ini:266-288`) that sets `-DEINK_DISPLAY_SINGLE_BUFFER_MODE=0` and `-DBOARD_X4PRO=1`, then gate the second-framebuffer allocation and double-buffer flip behind `#ifdef BOARD_X4PRO && !defined(EINK_DISPLAY_SINGLE_BUFFER_MODE)`. The second framebuffer would live in PSRAM (48KB), freeing DRAM.

**Risk:** High. The double-buffer flip logic and `storeBwBuffer()`/`restoreBwBuffer()` path need a full audit before committing. The `GfxRenderer.cpp:340-370` allocation path and the flip logic must be traced.

### F.2 OPI flash aware burst reads

The X4 Pro uses `board_build.arduino.memory_type = dio_opi` (`platformio.ini:270`) and `USE_BLOCK_DEVICE_INTERFACE=1` (`platformio.ini:287`). OPI (Octal Peripheral Interface) mode allows the flash to burst-read at higher speeds than standard SPI.

**Finding:** SdFat's block-device interface (`USE_BLOCK_DEVICE_INTERFACE=1`) already optimises SD reads for the X4 Pro's native SDMMC controller. The `FsFile` read pattern used by `ZipFile::readFileToStream` and `readItemContentsToStream` is already sequential (read-ahead friendly). **No specific change is needed for OPI burst reads** — the SdFat block-device path already exploits the hardware. The optimisation is to keep reads sequential (which Change E.1 achieves with larger chunk sizes) and avoid random small reads (which Change E.2 achieves with the ZIP cache).

### F.3 S3 hardware JPEG decoder probe (concrete answer)

**Finding:** `esp_jpeg_dec.h` exists in the Espressif `esp_new_jpeg` component cache (`~/.cache/Espressif/ComponentManager/service_d92d8f1e/espressif__esp_new_jpeg_1.0.2_e1e7e699/include/esp_jpeg_dec.h`). The header exposes `jpeg_dec_handle_t`, `esp_jpeg_decode()`, and block-decoder mode (`block_enable = true`).

**However:** the project uses `JPEGDEC` (`<JPEGDEC.h>`) from the FreeInk SDK, not `esp_jpeg_dec`. The `JPEGDEC` class (`JpegToFramebufferConverter.cpp:368,399`) is a C++ wrapper that provides `open()`/`close()`/`read()`/`seek()` callbacks and handles its own zlib decompression and scaling. The `esp_jpeg_dec` API is C-based (`esp_jpeg_decode(jpeg_dec_handle_t, ...)`) and would require:
1. A new `jpegOpen`/`jpegClose`/`jpegRead`/`jpegSeek` callback set that works with `esp_jpeg_dec`.
2. Replacing the `JPEGDEC` heap allocation with `esp_jpeg_dec` initialisation.
3. Handling the output format conversion (the hardware decoder outputs raw pixels; the current `JpegToBmpConverter` path expects a specific format).

**Conclusion:** The hardware decoder is available in the toolchain but not wired up. Integration is non-trivial and would be a Phase 4 change gated behind profiling. The `JPEGDEC` path works today and is the recommended baseline. **Do not propose adding `esp_jpeg_dec` integration in Phase 1.**

---

## 8. Migration Plan

Each phase is shippable independently and gated behind `#ifdef BOARD_HAS_PSRAM` (or `BOARD_X4PRO` where noted).

### Phase 1 — Safer wins (SD I/O reduction, no memory placement change)
1. **Change E.1:** Bump `readItemContentsToStream` chunk sizes to 8192 at `Epub.cpp:34,73,172,208,353,665,754`. Add `streamChunkSize = 8192` constant.
2. **Change E.5:** Debounce section `.bin` writes (`Section::suspendBuild()` at `Section.cpp:658-695`) — delay partial writes when the user is actively reading.
3. **Change E.6:** Confirm no-write metadata fast path (`book.bin` read-only during normal reading; trace all write paths in `BookMetadataCache.cpp:167`).
4. **Change E.2:** ZIP central-directory cache in PSRAM (`ZipFile::loadAllFileStatSlims()` at `ZipFile.cpp:64-113`) — persist the cache across `ZipFile` instances.

**Verification:** `pio run` for all envs; measure book-open time on X4 Pro; count `ZipFile::open()` calls before/after.

### Phase 2 — PSRAM caches for bulky structures
1. **Change D.1:** Keep active section's `Page` objects and LUT in PSRAM during build (`BuildContext` allocation via `makeUniqueNoThrow`).
2. **Change D.2:** Cover thumbnail buffer via `makeUniqueNoThrow` (`HomeActivity.cpp:166`).
3. **Change D.3:** PixelCache band buffer via `makeUniqueNoThrow` (`PixelCache.h:91`).
4. **Change D.4:** JPEGDEC/PNG decoder placement confirmation (`JpegToFramebufferConverter.cpp:368,399; PngToFramebufferConverter.cpp:352`).
5. **Change D.5:** ZIP central-dir cache (if not done in Phase 1).
6. Add `heap_free_kb` / `psram_free_kb` `LOG_INF` at book open and page turn.

**Verification:** `pio run -e x4pro` build size; device heap/PSRAM monitoring; page-turn latency for image-heavy chapters.

### Phase 3 — Dual-core parse task
1. **Change C.1:** Create a dedicated parse task pinned to core 0 (`xTaskCreatePinnedToCore(..., 0)`).
2. Add a `ParseQueue` (FreeRTOS `QueueHandle_t`) of `SpineIndex` values.
3. Modify `EpubReaderActivity::renderBook()` to send the `spineIndex` to the queue and wait for parse completion.
4. Implement PSRAM-resident section ring buffer (Section D.6) for pre-fetch.
5. Modify `Section::createSectionFile()` to write the `.bin` to PSRAM-resident storage.

**Verification:** `core_id()` `LOG_INF` at each pipeline stage; measure book-open time with/without dual-core; confirm render task stays responsive during build.

### Phase 4 — Hardware-aware render path
1. **Change F.1:** Add `[env:x4pro_perf]` to `platformio.ini` (`-DEINK_DISPLAY_SINGLE_BUFFER_MODE=0`); audit and implement double-buffer flip logic in `GfxRenderer` (lines ~120-131, ~340-370, ~2449-2504).
2. **Change F.2:** Confirm OPI burst-read optimisation (already in place via `USE_BLOCK_DEVICE_INTERFACE=1`); document.
3. **Change F.3:** Evaluate `esp_jpeg_dec` integration as a Phase 4+ option if profiling shows JPEG decode is the bottleneck.

**Verification:** Page-turn animation on device; `LOG_INF` heap before/after; watchdog monitoring.

---

## 9. Verification Plan

For each phase, verify with:

| Metric | Method | Baseline (today) | Target (Phase 2) |
|---|---|---|---|
| `heap_free_kb` at book open | `LOG_INF("MEM", "Free: %d", ESP.getFreeHeap())` | ~55KB (est.) | ≥80KB |
| `psram_free_kb` at book open | `LOG_INF("MEM", "PSRAM free: %d", ESP.getFreePsram())` | 0 (unused) | ≥6MB |
| `core_id()` at each stage | `LOG_INF("CORE", "Stage %s on core %d", stage, xPortGetCoreID())` | core 1 for parse+render | core 0 for parse, core 1 for render |
| `ZipFile::open()` count per book-open | `LOG_INF("ZIP", "ZipFile opens: %d", count)` | 15–30 | 1–2 |
| `pio run -e x4pro` build size | PlatformIO build output | baseline | ≤ baseline + 5KB |
| Page-turn latency (manually timed) | `millis()` delta between page requests | baseline | 20–30% reduction on image-heavy pages |
| Chapter re-open time | `millis()` delta | baseline | 200–500ms saved (D.1, E.1) |
| CI pass | `pio run` for all envs | pass | pass |

**Add to `src/main.cpp` (or the relevant activity):**
```cpp
LOG_INF("MEM", "Heap: %dKB, PSRAM: %dKB, Core: %d", ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024, xPortGetCoreID());
```
at book open and page-turn entry points.

**SD byte counters:** SdFat exposes some counters via the block-device API; add a `LOG_INF("SD", "Bytes read: %lu", sdBytesRead)` at book open and page turn if available.

---

## 10. Open Questions for the User

**(a) Is dual-core parse worth the complexity? — DECIDED 2026-09-03: proceed, gated on profiling.**
User decision (paraphrased): *“I think it should be, but we should profile.”* Today parse runs on core 1 inside the render task, blocking render during the 200–500 ms chapter build. The contention may be on PSRAM/cache bandwidth, not CPU — both tasks would allocate from PSRAM simultaneously. **Therefore the implementation is gated on a profiling step that must be completed before any dual-core code lands.** Profile with `core_id()` logging added to the chapter-build hot path (`Section::createSectionFile()` / `Section::buildSomeMore()`) and a counter of PSRAM bytes allocated per phase. If the profile shows contention on cache bandwidth rather than CPU, the dual-core change is still net-positive (it overlaps parse with the next page’s pre-fetch) but the implementation must account for PSRAM allocation contention. Move to Phase 3 once the profile is collected and reviewed.

**(b) Is the S3 hardware JPEG decoder available in arduino-esp32?**
**Yes** — `esp_jpeg_dec.h` is present in the Espressif `esp_new_jpeg` component cache. **But** the project uses `JPEGDEC` from the FreeInk SDK, not `esp_jpeg_dec`. Integration requires replacing the C++ wrapper and the callback-based I/O model. Available, but not wired up. Non-trivial integration; defer to Phase 4+ unless profiling shows JPEG decode is the bottleneck.

**(c) For the section `.bin` write cadence: is "settings-change only" already true, or is it also written on chapter-close?**
**Also written on chapter-close.** `Section::~Section()` calls `suspendBuild()` (`Section.cpp:85`) → `commitBuildFile(SECTION_FILE_PARTIAL_VERSION, ...)` (`Section.cpp:671`) if pages were built. The `.bin` is written on *every chapter close* (when the user navigates away or the reader exits). The "settings-change only" fast path does not exist today — the partial write always happens. Change E.5 proposes debouncing this.

**(d) Should we add a `pio run -e x4pro_perf` env at the same time as Phase 2, or defer?**
**Defer until Phase 4.** The `x4pro_perf` env requires `-DEINK_DISPLAY_SINGLE_BUFFER_MODE=0` and the double-buffer flip logic (GfxRenderer.cpp:120-131, ~340-370, ~2449-2504), which is not fully traced. Adding the env now would gate the single-buffer lift behind an unaudited code path. Add the env when Phase 4 begins, alongside the double-buffer audit.

**(e) Does the SDK fork's `ZipFile` already do any central-directory caching?**
**Partially.** `ZipFile` has a `fileStatSlimCache` (`ZipFile.h:45`) and `loadAllFileStatSlims()` (`ZipFile.cpp:64-113`) that builds the cache — but it is built per-`ZipFile` instance and discarded when the `ZipFile` is destroyed. There is no cross-instance cache. The `lastCentralDirPos` cursor (`ZipFile.h:48-49`) provides sequential-scan optimisation within a single `ZipFile` instance, but a fresh `ZipFile(filepath)` each call (`Epub.cpp:219,828-836,853-854`) discards it. Change E.2 (PSRAM central-dir cache) would make this cache persist across instances.

---

*Document ends. Path: `docs/design/2026-09-03-epub-engine-pipeline-and-io.md`*
