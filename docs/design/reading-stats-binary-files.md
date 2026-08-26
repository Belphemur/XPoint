# Reading Stats — Binary File Persistence (replaces SQLite design)

**Status:** Implemented on this branch (design + OpenCode optimization pass in §9).
**Target repo:** `Belphemur/crosspoint-reader` (branch `develop`)
**Source of the format:** `uxjulia/crossink` (`development`) binary stats files, adapted for CrossPoint.
**Primary build target:** `x4pro` (ESP32-S3, PSRAM, SDMMC, touch)
**Supersedes:** `reading-stats-sqlite-integration.md` (SQLite-backed store). That design was
implemented and merged (PR #1) plus hardened (PR #5), but on-device it fails deterministically:
`SQLITE_IOERR_SEEK` on rollback-journal I/O over SdFat/SDMMC (14.9 GiB free, all other writes
healthy). Root cause: SQLite's rollback journal issues small writes at alternating offsets
(header @0 → page records → nRec rewrite @0) — exactly the access pattern the SdFat-over-SDMMC
path handles worst. Decision: **drop SQLite entirely** and persist stats as plain versioned
binary files whose access pattern (one sequential write, atomic tmp+rename) this SD stack
handles reliably.

---

## 1. Goal & scope

1. Keep the shipped reading-stats feature set unchanged from the user's point of view:
   - Per-book stats (time read, pages, pace, completion, start/finish dates, time-of-day &
     day-of-week buckets).
   - Global / aggregate stats (totals, streaks, reading-history heatmap).
   - Existing viewer UIs (`BookStatsActivity`, global stats activity) keep working.
2. **Replace the persistence layer**: delete the SQLite store (`lib/SQLite/`,
   `Sqlite3Esp32` dep) and re-back the same in-memory structs with crossink-style
   versioned binary files:
   - Global: `/.crosspoint/global_stats.bin` (single fixed-size record, atomic rotate).
   - Per-book: `<cachePath>/stats_vN.bin` (fixed-size record living inside the book's cache dir).
3. Remove everything SQLite-specific that no longer makes sense (see §6).

**Out of scope (unchanged):** Nearby stats sync UI, KOReader/cloud sync, stats backup web flow.
The format keeps crossink's per-device aggregation seam so sync can be added later.

---

## 2. Why binary files win here (evidence)

| | SQLite + custom VFS | Binary files |
|---|---|---|
| Write pattern per commit | journal header/records/nRec at alternating offsets, then page writes, then journal delete (≈30 sector programs, ≈17.5 KB per session commit — §3.5) | one sequential `openFileForWrite` of a ≤160-byte record (≈3 sector programs per-book — §3.5) |
| Failure surface | custom VFS (~340 LoC, `SQLiteVfsHal.{h,cpp}`), journal semantics, page cache | none new — identical to settings/recent-books stores already shipping |
| On-device result | deterministic `SQLITE_IOERR_SEEK` on x4pro | crossink ships this exact scheme on the same hardware family |
| Flash footprint | ~880 KB amalgamation + VFS shim | ~200 LoC read/write helpers |
| RAM | page cache (128 KiB cap) + sqlite structs | one stack buffer of the record size (159 B max) |
| Crash safety | rollback journal replay | atomic `.tmp` → verify → rename (+ `.bak` rotate for globals) |

The HAL already proves the sequential-write pattern works: `PersistableStore` JSON saves,
recent-books store, and crossink's own `global_stats.bin` all use it daily.

---

## 3. Data model & on-disk layout

Two record types, both little-endian, version byte first, size-checked like crossink:

### 3.1 Per-book record — `<cachePath>/stats_v5.bin`

Crossink v5 is 73 bytes and already covers every field CrossPoint's merged feature exposes
(`estimatedTimeLeftSeconds`, pace fields, etc.). We ship **version 5 exactly** — same byte
value, same filename, same layout — so files are byte-for-byte interchangeable with crossink
in both directions (a v6 byte would fail crossink's exact `(size, version)` check and
contradict that very claim). Layout (73 bytes):

```
[0]      version (= 5)
[1-2]    sessionCount              uint16 LE
[3-6]    totalReadingSeconds       uint32 LE
[7-10]   totalPagesTurned          uint32 LE
[11]     isCompleted               uint8
[12-13]  avgSecondsPerForwardPage  uint16 LE
[14-15]  paceSampleCount           uint16 LE
[16]     flags                     bit0=startDateManual bit1=finishedDateManual
[17-18]  startDate.year            uint16 LE
[19]     startDate.month           uint8
[20]     startDate.day             uint8
[21-22]  finishedDate.year         uint16 LE
[23]     finishedDate.month        uint8
[24]     finishedDate.day          uint8
[25-40]  timeOfDaySeconds[4]       uint32 LE each
[41-68]  dayOfWeekSeconds[7]       uint32 LE each
[69-72]  estimatedTimeLeftSeconds  uint32 LE
```

Rationale for keeping the record inside the cache dir (**key decision**, differs from PR #1's
hashed `book_id` rows):
- Lifetime coupling is exact: the cache dir is created when the book first opens and deleted
  when the book/cache is removed → per-book stats are cleaned up with zero code
  (no `removeBook()`, no FK cascade, no orphan rows).
- The move-to-`/read` flow already renames the whole cache dir
  (`moveFinishedBookToReadFolder`, `EpubReaderActivity.cpp:132-159`, cache-dir rename at
  `:149`) → **stats migrate for free**; the entire `migrateBookKey()` machinery disappears.
  (`migrateMovedEpubState` does not exist in this repo.)
- One file per book, never enumerated: no directory scans, no global index to corrupt.
- Versioned filename (`stats_v5.bin`) + legacy fallback chain (v4 → unversioned `stats.bin`)
  gives forward-compatible migration, same discipline as crossink.

### 3.2 Global record — `/.crosspoint/global_stats.bin` (159 bytes)

Identical to crossink v3 layout (already byte-compatible with our merged schema):

```
[0]       version (= 3; see below)
[1-4]     totalSessions       uint32 LE
[5-8]     totalReadingSeconds uint32 LE
[9-12]    totalPagesTurned    uint32 LE
[13-16]   completedBooks      uint32 LE
[17-32]   timeOfDaySeconds[4] uint32 LE each
[33-60]   dayOfWeekSeconds[7] uint32 LE each
[61-64]   readingHistoryAnchorDay uint32 LE
[65-156]  readingHistoryBits[92]
[157-158] longestReadingStreak uint16 LE
```

Version byte adopts crossink's lineage at **3** (`CURRENT_FILE_VERSION` = 3,
`CURRENT_FILE_SIZE` = 159 in crossink `GlobalReadingStats.h` — the layout above is
byte-identical to it). Starting CrossPoint at 1 would be factually broken against §1's sync
seam: a real crossink v3 file copied over would read as `NewerFormat`, load as defaults, and —
because the newer-format guard permanently blocks saves — the user's global stats would be
frozen and silently discarded. Monotonicity is preserved (next bump is 4). Load accepts
`version <= 3` (sizes 13/17/159 per crossink's loader); anything larger ⇒ `NewerFormat` ⇒
refuse destructive saves (crossink behavior).

### 3.3 Write discipline (SD/HAL-optimized)

SdFat 2.3.1 semantics this section relies on (verified against `FatFile.cpp` / `FsFile.h`):

- `FatFile::close()` always calls `sync()` internally, and `FsFile::flush()` *is* `sync()`.
  `close()` is therefore the durability point; explicit `flush()`/`sync()` before `close()`
  re-does the same cache flush (near-no-op the second time, and each call re-takes the
  HalStorage `storageMutex`). **No explicit fsync is needed on either save path.**
- `SDCardManager::openFileForWrite` opens with `O_RDWR|O_CREAT|O_TRUNC`
  (`SDCardManager.cpp:337`): stale content and stale tmp files are handled by the open
  itself — no `exists()+remove()` pre-steps.
- A ≤159 B write at offset 0 of a freshly truncated file hits `CACHE_RESERVE_FOR_WRITE`:
  no pre-read of the data sector, a single 512-byte sector program; the dir entry + FAT are
  flushed once by `close()`'s `cacheSync()`. **Padding the record to 512 B buys nothing**
  (still exactly one sector and one cluster either way) and would break crossink's exact
  `(size, version)` validation — records stay unpadded.
- `FatFile::rename()` opens the destination with `O_CREAT|O_EXCL` → **it fails if the
  destination already exists**. The `.bak` rotation must `remove(.bak)` before renaming.

Per-book (`stats.save(cachePath)`):
- Build the full record on the stack (`uint8_t[73]`), single `Storage.openFileForWrite`,
  one `f.write(...) == 73` check, explicit `close()` (checked). No temp file, no flush/sync:
  worst case a torn write loses one book's counters, which reset gracefully on next load
  (size/version check). Cost: ≈3–4 sector programs (FAT free-list update from `O_TRUNC`, one
  data sector, dir entry) with the cluster reused in place — no create/delete churn.
- Write frequency unchanged: once per reading session (book close), not per page.

Global (`GlobalReadingStats::save()`): durability matters (all books aggregate here), so keep
crossink's tmp→rename atomicity, simplified for this HAL (three syncs → one):
1. `openFileForWrite("global_stats.bin.tmp")` — no stale-tmp pre-clean (`O_TRUNC` suffices);
2. serialize 159 B on the stack, one `write`, verify count == 159;
3. verify `f.fileSize() == 159` on the still-open handle (replaces crossink's post-close
   re-open; same information, zero extra I/O);
4. explicit `close()` — the single sync point (do not rely on the `HalFile` destructor here;
   the renames below must happen after the close);
5. `remove("global_stats.bin.bak")` if it exists (**required**: SdFat rename fails when the
   destination exists), then `rename(bin → bak)` if `bin` exists;
6. `rename(tmp → bin)`; on failure restore `bak → bin`.

`resetLocal()` writes the empty record via the same path and should also remove any stale
`.bak`, so a later torn-primary recovery cannot resurrect pre-reset totals (crossink leaves
the `.bak` behind — small hardening worth one extra call).

Every step is sequential whole-file I/O; there is no interleaved multi-file write pattern,
which is precisely what killed the embedded VFS path.

### 3.4 Reading paths

- Book open: try `stats_v5.bin` → `stats_v4.bin` → legacy `stats.bin`; validate
  `(size, version)` pairs exactly like crossink's loader — accepted pairs are
  (11,1), (12,2), (16,3), (69,4), (73,5); mismatch ⇒ defaults (fresh start).
- Global open: `global_stats.bin` → `global_stats.bin.bak`; version ≤ 3 accepted
  (sizes 13/17/159); `NewerFormat` blocks saves.
- Multi-device aggregation (`loadAggregated`) ports verbatim later with the synced-stats dir;
  saturating adds keep the merge overflow-safe.

### 3.5 Wear-leveling math (bytes written per reading session)

One reading session commits exactly one per-book save + one global save (book close,
`EpubReaderActivity::onExit`).

**SQLite baseline (PR #1 config: page 4096, journal DELETE, synchronous=NORMAL, 2 dirty pages
per commit — book + global upsert):**
- journal create (dir entry + FAT alloc) ≈ 2–3 sector programs
- journal header (512 B) = 1; two page records (4 B pgno + 4096 B page + 4 B checksum,
  sector-unaligned) ≈ 9 sector programs each = 18; commit-header rewrite = 1; journal fsync
  (NORMAL syncs the journal once)
- DB pages: 2 × 4096 B page-aligned = 8 sector programs; DB fsync
- journal delete (dir + FAT free) ≈ 2–3
- **≈30–35 sector programs, ≈17.5 KB payload**, plus per-commit create/delete metadata churn,
  at alternating offsets — the deterministic `SQLITE_IOERR_SEEK` failure pattern on x4pro.

**Binary scheme:**
- per-book: `O_TRUNC` free + 1 data sector + dir entry ≈ 3–4 sector programs (73 B payload,
  cluster reused in place, single sync via `close()`)
- global: tmp create+write+close ≈ 4, remove `.bak` ≈ 2, two renames ≈ 2 dir sectors ≈ 8
  (159 B payload)
- **≈11–12 sector programs, ≈5 KB total** (FAT32 metadata dominates the payload), all
  sequential.

**≈3× fewer sector programs, ≈3.5× fewer bytes per session.** At 5 sessions/day that is
≈32 MB/yr → ≈9 MB/yr — endurance is a non-issue either way on any SD card; the reliability
win (no alternating-offset small writes, no per-commit create/delete) is what matters.

---

## 4. Code changes

### Deleted (complete SQLite removal)
| Path | Action |
|---|---|
| `lib/SQLite/` (`ReadingStatsStore.*`, `SQLiteVfsHal.*`) | **delete directory** |
| `siara-cc/Sqlite3Esp32 @ 2.5.0` in `platformio.ini` `lib_deps` | remove |
| `test/reading_stats/ReadingStatsStoreTest.cpp` + its CMake target | delete (replaced by binary round-trip tests) |
| Store singleton (`#define ReadingStats`), `begin()` default path `/.crosspoint/reading_stats.db` | replaced by direct load/save calls (there is no `READING_STATS_DB_PATH` symbol; the path is a default arg) |
| `bookStatsDbKey()` in `ReadingStatsUtils.{h,cpp}` + `bookId_` member in `EpubReaderActivity` | delete (unused once the stats key is `cachePath`) |
| `reading_sessions` table / `recordSession()` | dropped without replacement — zero call sites in `src/` today (dead API) |

### Re-implemented (same public shape where possible)
| File | Change |
|---|---|
| `src/activities/reader/BookReadingStats.{h,cpp}` | swap SQL body for crossink's binary load/save/remove keyed by `cachePath`; math/buckets unchanged |
| `src/activities/reader/GlobalReadingStats.{h,cpp}` | swap SQL body for the atomic binary load/save incl. backup rotation + newer-format guard |
| `src/activities/reader/EpubReaderActivity.cpp` | drop `ReadingStatsStore::getInstance()` usage & `bookStatsDbKey`/`migrateBookKey`; call `stats.save(epub->getCachePath())` on session commit (mirrors crossink call sites); move-to-`/read` needs **no** stats handling (dir rename carries it) |
| `test/reading_stats/` | new host tests: v5 round-trip, legacy-fallback chain, torn-file rejection (short/garbage), global tmp+bak rotation incl. failure injection, NewerFormat guard |

### Kept as-is
`ReadingStatsUtils` (pure date/bucket/history math, minus `bookStatsDbKey()`), both stats
activities/UI, settings gating (`READING_STATS_ENABLED` on the `x4pro` + `papermono` envs —
`sticky` is also N16R8/PSRAM but lacks the flag — plus the
`SETTINGS.shouldTrackReadingStats()` runtime toggle), session lifecycle thresholds
(≥10 s count, ≥60 s = session).

## 5. HAL cleanup (SQLite-era additions)

Repo reality (verified 2026-08-26): **PR #5 was closed unmerged** — its changes are not in
this tree. The only HAL additions that actually landed came with PR #1 (`015f14e9`).

| Change | Status in repo | Verdict | Reason |
|---|---|---|---|
| `HalFile::truncate(uint64_t)` / `HalFile::sync()` (`HalStorage.h:84-85`) | present (PR #1) | **keep** | generic HAL completeness (SdFat primitives under `storageMutex`); zero cost when unused. §3.3 no longer needs explicit `sync()` — `close()` syncs |
| `HalStorage::{sdTotalBytes,sdUsedBytes}` wrappers | **never landed** (PR #5 only) | no-op | they exist solely in freeink-sdk's `SDCardManager` (cached, 20 s TTL); if a future storage-info UI wants them, wrap then |
| `SQLiteVfsHal.cpp` journal buffer / IOERR logging / NOENT mapping | present (PR #1) | **delete with lib/** | meaningless without the VFS |
| extended-result-code logging, low-disk recreate guard, bounded `begin()` | PR #5 items, never merged | moot | whole `lib/SQLite/` is deleted; note the in-tree `ReadingStatsStore::begin()` retries via *unbounded* recursion (`ReadingStatsStore.cpp:101,117`) — deleted, not fixed |

Net: `lib/hal` keeps two small, genuinely reusable forwards; everything else SQLite-flavored is
removed root and branch ("nothing will stay").

## 6. Testing & verification

1. Host CTest (fast): round-trip v5 per-book record; legacy v4/`stats.bin` fallback; garbage/short
   file ⇒ fresh defaults; global save produces `.bin` + `.bak`, recovery from `.bak` when `.bin`
   is truncated; NewerFormat refuses overwrite; `recordReadingSpan` bucket math unchanged.
   Rotation tests must cover SdFat semantics: rename with the destination already present
   (stale `.bak`) succeeds via the explicit pre-remove, and a stale `.tmp` from a killed write
   is overwritten cleanly (no pre-clean step).
2. `pio run -e x4pro` (hard gate) + `-e default` to prove non-PSRAM stub still compiles.
3. Simulator smoke test pass.
4. On-device: fresh boot creates no `reading_stats.db`; read session persists across reboot;
   finishing a book moves stats to `/read` cache dir intact (`READ_FOLDER`,
   `EpubReaderActivity.cpp:74`); disabling tracking writes nothing.

## 7. Risks

- **R1:** losing PR #1 users' existing SQLite data (there are essentially none outside dev
  devices; the DB was broken on-device anyway). No migration path provided; note in changelog.
  In the other direction, version alignment (book v5, global v3) means crossink-produced files
  load natively — no shim needed there either.
- **R2:** two writers to the same book file can't happen (single reader task, sessions don't
  overlap); global file likewise written only from main task — same invariant the stores already rely on.
- **R3:** FAT sector overhead means each 73-byte save rewrites a cluster — identical cost class
  to every other small store on the card; frequency (per session close) is unchanged from PR #1.
  Per-session wear drops ≈3× vs the SQLite baseline (§3.5).

## 8. OpenCode optimization pass (planned before implementation)

Run the finalized doc through OpenCode (`opencode-go/kimi-k3`) with repo access, asking
specifically for:
1. SD/HAL-level optimizations of the format/write path (alignment to 512-byte sectors?
   pack global record tighter? skip `.bak` rotation when dirty-byte-count is trivial?)
2. Anything in the crossink-sourced code worth simplifying for CrossPoint's HAL
   (`openFileForWrite` already truncates; redundant `exists()+remove()` pre-steps?).
3. Wear-leveling sanity check: bytes-per-session estimate vs PR #1 baseline.
Findings get folded into this doc before implementation starts.

**Update 2026-08-26:** the pass ran; findings are folded into §§2–7 and summarized in §9.

---

## 9. OpenCode optimization pass (applied)

Pass ran 2026-08-26 against this repo (`feat/reading-stats-binary`) and crossink
(`uxjulia/crossink @ development`). Changes applied above, with the reason for each:

1. **Per-book version 6 → 5, file `stats_v5.bin` (§3.1, §3.4, §4, §6).** The doc claimed
   "byte-for-byte crossink-v5 compatible" while bumping the version byte to 6 — a
   contradiction: real crossink v5 files would fail the exact `(size, version)` check.
   Shipping v5 verbatim makes the claim true and keeps the fallback chain identical to
   crossink (v5 → v4 → `stats.bin`).
2. **Global version 1 → 3 (§3.2, §3.4).** With a byte-identical 159-byte layout, version 1
   would make genuine crossink v3 files read as `NewerFormat`, which permanently blocks
   saves — the user's stats would freeze and §1's sync seam would be dead on arrival.
   Monotonicity is preserved (next bump is 4).
3. **Write path simplified (§3.3).** Dropped explicit `flush()`/`sync()` (SdFat's `close()`
   always syncs; `flush()` *is* `sync()` — verified in `FatFile.cpp`/`FsFile.h`); dropped the
   stale-tmp pre-clean (`openFileForWrite` is `O_TRUNC`, `SDCardManager.cpp:337`); replaced
   crossink's post-close re-open size verify with an in-handle `fileSize()` check before
   `close()`; documented that `remove(.bak)` before rotation is *required* because
   `FatFile::rename()` fails when the destination exists (`O_EXCL`). No per-book fsync: torn
   writes self-heal via the `(size, version)` check. **Answer to the §8 alignment question:
   no 512-byte padding** — a ≤159 B record already costs exactly one sector program (read-free
   via `CACHE_RESERVE_FOR_WRITE`) and one cluster; padding only breaks crossink's exact-size
   validation. **Answer to the §8 `.bak` question: keep the rotation** — it costs ≈4 extra
   sector programs per session and protects the one file that aggregates all books; skipping
   it saves nothing measurable and loses the torn-write recovery path.
4. **Wear math made explicit (new §3.5).** ≈17.5 KB in ~30 alternating-offset sector programs
   per SQLite session commit vs ≈5 KB in ~11 sequential sector programs for the binary scheme;
   ≈3× fewer programs, ≈3.5× fewer bytes. The real win is the *pattern* (no journal
   create/delete churn, no alternating offsets — the deterministic `SQLITE_IOERR_SEEK`
   trigger), not the volume.
5. **Factual corrections from repo verification.**
   - Custom VFS is ~340 LoC, not ~500 (§2).
   - `READING_STATS_DB_PATH` does not exist; the actual artifacts are the `begin()` default
     path and the `#define ReadingStats` macro (§4).
   - `migrateMovedEpubState` does not exist in this repo; the real call site is
     `moveFinishedBookToReadFolder` (`EpubReaderActivity.cpp:132-159`) (§3.1).
   - `/Read` → `/read` (`READ_FOLDER`, `EpubReaderActivity.cpp:74`) (§3.1, §4, §6).
   - **PR #5 is closed unmerged**; §5 rewritten — `HalFile::truncate`/`sync` came from PR #1
     (`015f14e9`) and stay; `HalStorage::{sdTotalBytes,sdUsedBytes}` never landed (they live
     only in freeink-sdk's `SDCardManager`); the rest is moot once `lib/SQLite/` is deleted.
   - `recordSession()` / the `reading_sessions` table have zero call sites in `src/` —
     dropped without replacement (§4).
   - `READING_STATS_ENABLED` is set on the `x4pro` + `papermono` envs, not "PSRAM envs"
     (`sticky` is N16R8/PSRAM but lacks the flag) — §4 wording corrected.

**Deliberately kept intact:** no-temp per-book write; `.bak` rotation for the global record;
the `NewerFormat` destructive-save guard; legacy fallback chains; ≥10 s / ≥60 s thresholds;
`READING_STATS_ENABLED` gating; the cache-dir-coupled lifetime model; `loadAggregated` as a
later port.
