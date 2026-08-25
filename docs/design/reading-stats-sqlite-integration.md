# Reading Stats (SQLite-backed) + Font Downloader Integration

**Status:** Design / architecture plan — pending OpenCode review.
**Target repo:** `Belphemur/crosspoint-reader` (fork of `crosspoint-reader/crosspoint-reader`, branch `develop`)
**Source of features:** `uxjulia/crossink` (branch `development`)
**Primary build target:** `x4pro` (ESP32-S3, PSRAM, SDMMC, touch)

---

## 1. Goal & scope

1. **Port the CrossInk "Reading Stats" feature into CrossPoint.**
   - Per-book stats (time read, pages, pace, completion, start/finish dates, time-of-day & day-of-week buckets).
   - Global / aggregate stats (totals, streaks, reading-history heatmap).
   - Stats viewer UI (book stats activity + global stats activity), reachable from the reader menu and settings.
2. **Re-back the stats store with SQLite instead of CrossInk's per-book binary files (`stats_v5.bin`) and global binary file (`global_stats.bin`).**
   - One SQLite database file on the SD card replaces the many small `.bin`/JSON files.
3. **Integrate (reconcile) the CrossInk font-downloader improvements into CrossPoint's existing `FontDownloadActivity`.**
   - CrossPoint already has a working font downloader (873 LoC); CrossInk's is more advanced (1372 LoC: family management, update-all, delete, CRC validation). We port the *improvements*, not a wholesale rewrite.
4. **Build the `x4pro` firmware with PlatformIO to prove the integration compiles and links.**

**Out of scope (this pass):** CrossInk's Nearby stats sync (`NearbyStatsSyncActivity`), KOReader/cloud sync, and the stats backup web flow. These depend on networking/sync infra we are not porting now. The schema and data model are designed so sync can be added later (e.g. an `export`/`import` table or row versioning), but no sync UI is built yet.

---

## 2. Current state (ground truth from the two trees)

### CrossPoint (`develop`, commit `8665dd7`)
- **No reading stats at all.** Grep for `GlobalReadingStats|BookReadingStats|recordReadingSpan|recordForwardPageRead` → 0 matches in `src/`.
- Has the **full font-downloader stack already**: `src/activities/settings/FontDownloadActivity.{h,cpp}`, `src/FontInstaller.{h,cpp}`, `src/SdCardFontSystem.{h,cpp}`, `lib/EpdFont/SdCardFont*`.
- SD is owned by `HalStorage` → `SDCardManager` (freeink-sdk), using **SdFat** (SPI on C3, SDMMC-block-device on x4pro via `-DUSE_BLOCK_DEVICE_INTERFACE=1`). There is **no POSIX VFS mount** (no `esp_vfs_fat`), so paths like `/sd/...` do **not** exist.
- Clock offset setting `clockUtcOffsetQ` exists (`CrossPointSettings.h:216`) — CrossInk's `getCurrentLocalReadingStatsDateTime()` (which reads `SETTINGS.clockUtcOffsetQ`) ports cleanly.
- Native CMake test suite under `test/`, plus a simulator profile; `scripts/run_simulator_smoke_test.py` is the app-flow tripwire.

### CrossInk (`development`, commit `5ef1905`)
- Full reading-stats subsystem:
  - `src/activities/reader/ReadingStatsUtils.{h,cpp}` — date math, buckets, history bitmaps, formatting (pure, portable).
  - `src/activities/reader/GlobalReadingStats.{h,cpp}` — aggregate stats, persisted to `/.crosspoint/global_stats.bin` (159-byte v3 binary).
  - `src/activities/reader/BookReadingStats.{h,cpp}` — per-book stats, persisted to `<cachePath>/stats_v5.bin` (73-byte v5 binary).
  - `src/activities/reader/BookStatsActivity.{h,cpp}`, `BookStatsView.{h,cpp}` — UI.
  - `src/activities/reader/StatsBackup.cpp` — backup helper.
  - Wiring in `EpubReaderActivity.cpp`: session start/stop, `recordReadingSpan`, `recordForwardPageRead`, completion prompt, "Reading stats" menu action, delete-stats handling.
- Font downloader is more advanced than CrossPoint's (family management, update-all, delete, CRC32 validation).

### Key takeaway
CrossPoint has **none** of the stats code; we add the whole subsystem. The font downloader already exists and only needs targeted enhancement.

---

## 3. Decisions (the architectural calls)

### D1 — SQLite library: `siara-cc/Sqlite3Esp32`
- PlatformIO registry: `siara-cc/Sqlite3Esp32` (v2.5, Apache-2.0). It bundles a pre-patched `sqlite3.c` amalgamation already adapted for the ESP32 (no extra porting of SQLite itself).
- Added via `lib_deps` in `platformio.ini` (gated to the `x4pro`/PSRAM-family envs — see D4).

### D2 — The hard part: how SQLite reaches the SD card (custom VFS over `HalStorage`)
The stock `Sqlite3Esp32` opens databases through an **esp-idf POSIX VFS** (e.g. `/sd/stats.db`). CrossPoint does **not** register such a VFS — it talks to SdFat through `HalStorage`/`SDCardManager`. Two options:

- **Option A (CHOSEN): custom SQLite VFS `"hal"` that routes every I/O call through `HalStorage`/`HalFile`.**
  - `sqlite3_vfs.xOpen/xRead/xWrite/xTruncate/xSync/xFileSize/xLock/xUnlock/xShm*...` delegate to `Storage.openFileForRead/Write`, `HalFile::read/write/seek/size/close`.
  - **Benefits:** single owner of the SD card (no double-mount corruption), inherits `HalStorage`'s recursive mutex (thread-safe across the reader task and render task), identical behavior on ESP32-C3 (SdFat SPI) and ESP32-S3 (SDMMC), and disjoint from any future `esp_vfs_fat` use.
  - **Cost:** ~300–500 LoC VFS shim; locking/shm semantics are the subtle part (see D3/WAL).
- **Option B (REJECTED): mount the same card a second time via `esp_vfs_fat_sdmmc` just for SQLite.**
  - Two FAT drivers on one volume = cache/consistency hazard, and on x4pro the SDMMC host is already owned by `SDCardManager`. Rejected as unsafe.

**Host/test portability:** the VFS shim is the only device-specific piece. The native CMake test suite (runs on the host) will back the *same* `sqlite3_vfs` with POSIX `open/read/write` (std::fstream or `<cstdio>`), so all schema/stats logic is unit-testable on the desktop without flashing a device. Only the I/O backend differs between host and device.

### D3 — Journal mode: rollback (DELETE), NOT WAL  *(user decision, 2026-08-25)*
**Decision: do NOT use WAL.** Use SQLite's default rollback journal (`PRAGMA journal_mode=DELETE`) with `PRAGMA synchronous=NORMAL`. Rationale:
- **Single writer, no concurrency.** Stats are written only from the main/reader task (book-close / completion / dwell commits). The render task never touches the DB. WAL only pays off when a reader and writer run concurrently — which never happens here.
- **SD-card portability.** WAL leaves `-wal`/`-shm` side files that must be checkpointed or data stays split; if the card is yanked/read by a PC before checkpoint, data is stranded. DELETE journal writes only the `.db` and auto-removes its temp `-journal` after commit → one self-contained file. Exactly what you want on a removable SD.
- **Risk removal.** WAL forces implementing `xShmMap/xShmUnmap/xShmLock/xShmBarrier` — the riskiest part of the custom VFS. Dropping WAL removes that entire surface (`xShm*` can return `SQLITE_OK`/unsupported; no `-shm` file needed).
- Writes are infrequent (per book-close, not per page), so rollback journal's "rewrite whole page on commit" cost is negligible here.

PRAGMAs (embedded-tuned):
- `PRAGMA journal_mode=DELETE` (explicit; the default, but set to be clear).
- `PRAGMA synchronous=NORMAL` — the app crashes/loses power only between commits (which are atomic under rollback journal); NORMAL avoids full fsync per transaction while staying safe for this access pattern.
- `PRAGMA foreign_keys=ON`, `PRAGMA temp_store=MEMORY` (no temp b-tree churn on SD), `PRAGMA mmap_size=0` (no mmap on SD), `PRAGMA page_size=4096`, `PRAGMA cache_size=-128` (~512 KB, PSRAM-backed on x4pro).
- `PRAGMA integrity_check` after any unclean recovery / on open-failure path.
- **Placement:** DB file `/.crosspoint/reading_stats.db`. Heap for the page cache from PSRAM on x4pro (`BOARD_HAS_PSRAM`).

**Host/test portability:** same `sqlite3_vfs` "hal" abstraction (D2); the host backend uses POSIX files. Without WAL there is no `-shm`, so the host VFS is much simpler too.

### D3b — VFS primitive gaps to close first  *(OpenCode review: blocking)*
`HalFile` (`lib/hal/HalStorage.h:61-98`, `HalStorage.cpp`) does **not** yet expose two primitives the VFS needs:
- **`truncate()`** — required by `xTruncate` (needed for `VACUUM`/temp-file growth). SdFat's `FsFile::truncate(uint64_t)` exists but is not forwarded. **Add `HalFile::truncate(uint64_t size)`** (acquire `storageMutex`, call `impl->file.truncate(size)`).
- **`sync()`** — `xSync(SQLITE_SYNC_FULL)` needs SdFat's `FsFile::sync()` (also flushes FAT/directory entries), not just `flush()`. **Add `HalFile::sync()`** (acquire `storageMutex`, call `impl->file.sync()`).
- **Seek/read atomicity:** each `HalFile` method takes the recursive `storageMutex` separately, so a `seek()`+`read()` pair is two lock holds with a gap. The single-connection invariant above makes this safe; document it. If a future second connection appears, the shim needs its own compound lock.

### D4 — Feature / capability gating  *(correction C2/C3 from OpenCode review)*
- SQLite + the stats subsystem are **memory-heavy** (SQLite page cache, WAL files). Keep them off the constrained ESP32-C3 default target **and off `sticky`** (the `sticky` env in `platformio.ini:198` deliberately has **no** `-DBOARD_HAS_PSRAM` and no `-DUSE_BLOCK_DEVICE_INTERFACE=1`).
- Gate on the **`BOARD_HAS_PSRAM`** macro — *not* a hard-coded device list. Only `x4pro*` (`platformio.ini:242,261,275`) and `papermono*` (`:295,310,323`) define it; `sticky` and `default` do not. The `base` env sets `-DREADING_STATS_ENABLED=1` **only when `BOARD_HAS_PSRAM` is defined**, e.g.:
  - `build_flags = ${base.build_flags} -DREADING_STATS_ENABLED=$($BOARD_HAS_PSRAM ? 1 : 0)` style, or simpler: each PSRAM env adds `-DREADING_STATS_ENABLED=1` and the `default`/`sticky` envs omit it.
- On non-enabled targets, `ReadingStatsStore` compiles to a no-op stub and the menu entries are hidden. This mirrors how CrossPoint already gates features via `BoardConfig::hasTouch()` / `FREEINK_DEVICE_*` / `BOARD_HAS_PSRAM` macros (`src/SettingsList.h:461,475`, `SettingsActivity.cpp:82`) — there is **no `AppCapabilities` class**; use `BoardConfig`/macros, not `AppCapabilities`.
- **Runtime opt-out:** add `SETTINGS.shouldTrackReadingStats()` (default on). No `shouldTrack*` precedent exists today; this is new ground but matches CrossInk's `SETTINGS.shouldTrackReadingStats()` usage.
- Font downloader needs **no** new gating (already device-agnostic).

### D5 — Font downloader: verify, don't port  *(correction C1 from OpenCode review)*
**Premise correction:** CrossPoint at `develop` HEAD **already ships** every feature the original plan assumed was CrossInk-only. Confirmed by reading the actual files:
- Family management (`ManifestFamily`, `FAMILY_LIST`/`GROUP_LIST` states) — `FontDownloadActivity.h:45-53,61`
- Update-all — `FontDownloadActivity.cpp:268,282`
- Delete — `promptDeleteSelectedFamily():545`, `onDeleteConfirmationResult():558`
- CRC32 validation — `computeFileCrc32():400`, manifest `crc32` field `:220-225`

The "873 vs 1372 LoC" framing was misleading: whatever CrossInk adds beyond 873 lines is **not** these features, and CrossInk's extra lines use a *different UI base* (`FreeInkApp`) and a *plain-HTTP S3 manifest* that are both **wrong for CrossPoint** (CrossPoint uses HTTPS GitHub Releases — `FontDownloadActivity.h:23-25`).

**This task becomes a verification pass, not an implementation:**
1. Run a line-level diff of `FontDownloadActivity.{h,cpp}` + `FontInstaller.{h,cpp}` + `SdCardFont*` between the two trees.
2. Expected outcome: **no functional port needed** (features already present).
3. Port only genuinely-new, CrossPoint-safe items discovered in the diff — and **never** CrossInk's file wholesale, its plain-HTTP manifest, or its `FreeInkApp` UI base.
`FreeInkApp.h` *does* exist in this freeink-sdk pin (`FreeInkUI/include/FreeInkApp.h`), so the earlier worry about a missing library is resolved; the remaining risk is UI-base drift, not a missing dependency.

---

## 4. Data model (SQLite schema)

Replaces CrossInk's two binary files with tables in `/.crosspoint/reading_stats.db`.

```sql
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
PRAGMA user_version = 1;   -- future migration hook (aligns with CrossPoint's
                            -- cache-versioning discipline: book.bin v10 / section.bin v41)

-- One row per device/install; id is fixed (1) for the local device.
CREATE TABLE IF NOT EXISTS global_stats (
  id                          INTEGER PRIMARY KEY CHECK (id = 1),
  total_sessions              INTEGER NOT NULL DEFAULT 0,
  total_reading_seconds       INTEGER NOT NULL DEFAULT 0,
  total_pages_turned          INTEGER NOT NULL DEFAULT 0,
  completed_books             INTEGER NOT NULL DEFAULT 0,
  tod_morning                 INTEGER NOT NULL DEFAULT 0,  -- READING_TIME_BUCKET_COUNT=4
  tod_afternoon               INTEGER NOT NULL DEFAULT 0,
  tod_evening                 INTEGER NOT NULL DEFAULT 0,
  tod_night                   INTEGER NOT NULL DEFAULT 0,
  dow_mon                     INTEGER NOT NULL DEFAULT 0,  -- READING_DAY_OF_WEEK_COUNT=7
  dow_tue                     INTEGER NOT NULL DEFAULT 0,
  dow_wed                     INTEGER NOT NULL DEFAULT 0,
  dow_thu                     INTEGER NOT NULL DEFAULT 0,
  dow_fri                     INTEGER NOT NULL DEFAULT 0,
  dow_sat                     INTEGER NOT NULL DEFAULT 0,
  dow_sun                     INTEGER NOT NULL DEFAULT 0,
  reading_history_anchor_day  INTEGER NOT NULL DEFAULT 0,
  reading_history_bits        BLOB NOT NULL,   -- 92 bytes (READING_HISTORY_BYTES)
  longest_reading_streak      INTEGER NOT NULL DEFAULT 0
);

-- One row per book. book_id = the book's cache-path string (see §4 note), NOT a
-- bare std::hash (implementation-defined across libstdc++/libc++ and not collision-free).
CREATE TABLE IF NOT EXISTS book_stats (
  book_id                     TEXT PRIMARY KEY,
  session_count               INTEGER NOT NULL DEFAULT 0,
  total_reading_seconds       INTEGER NOT NULL DEFAULT 0,
  total_pages_turned          INTEGER NOT NULL DEFAULT 0,
  is_completed                INTEGER NOT NULL DEFAULT 0,
  avg_seconds_per_forward_page INTEGER NOT NULL DEFAULT 0,
  pace_sample_count           INTEGER NOT NULL DEFAULT 0,
  estimated_time_left_seconds INTEGER NOT NULL DEFAULT 0,
  start_date_manual           INTEGER NOT NULL DEFAULT 0,
  finished_date_manual        INTEGER NOT NULL DEFAULT 0,
  start_date_year             INTEGER NOT NULL DEFAULT 0,
  start_date_month            INTEGER NOT NULL DEFAULT 0,
  start_date_day              INTEGER NOT NULL DEFAULT 0,
  finished_date_year          INTEGER NOT NULL DEFAULT 0,
  finished_date_month         INTEGER NOT NULL DEFAULT 0,
  finished_date_day           INTEGER NOT NULL DEFAULT 0,
  tod_morning                 INTEGER NOT NULL DEFAULT 0,
  tod_afternoon               INTEGER NOT NULL DEFAULT 0,
  tod_evening                 INTEGER NOT NULL DEFAULT 0,
  tod_night                   INTEGER NOT NULL DEFAULT 0,
  dow_mon                     INTEGER NOT NULL DEFAULT 0,
  dow_tue                     INTEGER NOT NULL DEFAULT 0,
  dow_wed                     INTEGER NOT NULL DEFAULT 0,
  dow_thu                     INTEGER NOT NULL DEFAULT 0,
  dow_fri                     INTEGER NOT NULL DEFAULT 0,
  dow_sat                     INTEGER NOT NULL DEFAULT 0,
  dow_sun                     INTEGER NOT NULL DEFAULT 0
);

-- Optional, forward-looking: per-session log enables richer queries and
-- future "Nearby sync" export without touching the aggregate tables.
CREATE TABLE IF NOT EXISTS reading_sessions (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  book_id       TEXT NOT NULL,
  start_epoch   INTEGER NOT NULL,
  duration_sec  INTEGER NOT NULL,
  pages_turned  INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY (book_id) REFERENCES book_stats(book_id)
);
CREATE INDEX IF NOT EXISTS idx_sessions_book ON reading_sessions(book_id);
```

- **`book_id` (correction C4):** key on the **full cache-path string** used by CrossPoint's EPUB cache (the directory that currently holds `stats_v*.bin`), e.g. `epub->getCachePath()`. Do **not** key on the bare `std::hash<std::string>` (`EpubReaderActivity.cpp:125`) — it is implementation-defined and not collision-free. The full path is already stable per book and survives re-opens. (If path length is a concern, hash it with a documented, stable algorithm + store the path too, but the PK stays the path string.)
- The `ReadingStatsUtils` date/bucket/history-bitmap helpers are **reused verbatim** (they are pure C++, no file I/O). The bitmap (anchor + 92-byte `reading_history_bits`) is stored as a `BLOB` rather than split — keeping CrossInk's anchor-day + `READING_HISTORY_BYTES` streak math byte-for-byte intact (no normalization win from splitting; it only adds write amplification). `GlobalReadingStats::recordReadingSpan` keeps using `recordReadingSpanIntoHistory` etc.; only the *load/save* path changes to SQL.
- **`saveGlobal()` uses an UPSERT** (`INSERT ... ON CONFLICT(id) DO UPDATE`) instead of read-then-write, removing a read round-trip and a race window.
- Bucket columns are **4 + 7 plain INTEGERs** (greenfield schema) rather than JSON text — cheaper to read/write and indexable.
- Migration: there is **no prior data** in CrossPoint, so no upgrade path is needed. `user_version` is set for future migrations only.

---

## 5. New / changed files (implementation plan)

### Reading stats — new (`src/activities/reader/` + `lib/`)
| File | Purpose |
|------|---------|
| `lib/SQLite/SQLiteVfsHal.h/.cpp` | Custom `sqlite3_vfs` "hal" over `HalStorage`; host build uses a POSIX backend behind the same interface (so `test/` can exercise it). `xDeviceCharacteristics` reports `xSectorSize=512` and **no** `ATOMIC`/`POWERSAFE_OVERWRITE`. DELETE journal (no `xShm*`). Single-connection invariant documented. |
| `lib/SQLite/ReadingStatsStore.h/.cpp` | Opens `/.crosspoint/reading_stats.db`, runs PRAGMAs (`journal_mode=DELETE`, `foreign_keys`, `user_version=1`, `synchronous=NORMAL`, `temp_store=MEMORY`, `mmap_size=0`, `page_size=4096`, `cache_size` capped to ~-128 ⇒ 512 KB), `integrity_check` on recovery. Provides `loadGlobal()/saveGlobal()` (UPSERT), `loadBook(id)/saveBook(id)/removeBook(id)`, `recordSession(...)`. Single lazily-opened `sqlite3*` handle, used only from the main/reader task. |
| `src/activities/reader/ReadingStatsUtils.{h,cpp}` | Copied from CrossInk (pure helpers, no file I/O). |
| `src/activities/reader/GlobalReadingStats.{h,cpp}` | Rewritten `load()/save()/recordReadingSpan()` to read/write the `global_stats` row via `ReadingStatsStore` instead of `global_stats.bin`. Bucketing/streak math unchanged. `save()` is an UPSERT. |
| `src/activities/reader/BookReadingStats.{h,cpp}` | Rewritten `load(cachePath)/save()/remove()` to map to the `book_stats` row keyed by the cache-path string. Pace/math unchanged. |
| `src/activities/reader/BookStatsActivity.{h,cpp}` + `BookStatsView.{h,cpp}` | Copied from CrossInk (UI). |
| `src/activities/settings/GlobalStatsActivity.{h,cpp}` | Copied/adapted from CrossInk's global stats viewer. |

### Reading stats — changed
| File | Change |
|------|--------|
| `lib/hal/HalStorage.{h,cpp}` | **Add `HalFile::truncate(uint64_t)` and `HalFile::sync()`** (forward to SdFat `FsFile::truncate`/`sync`, under `storageMutex`). Required by the VFS for WAL checkpoint. |
| `src/activities/reader/EpubReaderActivity.cpp` | Port CrossInk's session lifecycle (`onEnter` load + session start; `onExit` commit with ≥10s/≥60s thresholds; `recordReadingSpan` + `recordForwardPageRead` on dwell; completion-prompt trigger; `READING_STATS` + `DELETE_STATS` menu actions). Guarded by `READING_STATS_ENABLED`. `book_id` = `epub->getCachePath()`. |
| `src/activities/reader/TxtReaderActivity.cpp` / `XtcReaderActivity.cpp` | Same session wiring if CrossPoint tracks their reading (mirror CrossInk's EPUB+XTC support). |
| `src/activities/reader/EpubReaderMenuActivity.h` | Add `READING_STATS` (and `DELETE_STATS` if not present) to `MenuAction`, gated by `READING_STATS_ENABLED`. |
| `src/SettingsList.h` / settings activity | Add "Reading Stats" entry (global stats viewer) when enabled. |
| `src/CrossPointSettings.h` | Add `shouldTrackReadingStats()` flag (default on for enabled devices) so users can disable tracking. |
| `src/I18n/...` (translations) | Add strings for the new menu/stats UI (port CrossInk's `STR_*` ids used by `BookStats*`). |

### Font downloader — verification pass only  *(C1: features already present)*
| File | Change |
|------|--------|
| `src/activities/settings/FontDownloadActivity.{h,cpp}` | **No port.** Run a line-level diff vs CrossInk; expected outcome = no change. Do **not** adopt CrossInk's file, plain-HTTP manifest, or `FreeInkApp` UI base. |
| `src/FontInstaller.{h,cpp}`, `lib/EpdFont/SdCardFont*` | Diff only; port solely genuinely-new, CrossPoint-safe items. |

### Build / config
| File | Change |
|------|--------|
| `platformio.ini` | Add `siara-cc/Sqlite3Esp32` to `lib_deps`. Add `-DREADING_STATS_ENABLED=1` **only** to the `BOARD_HAS_PSRAM` envs (`x4pro*`, `papermono*`) — **not** `sticky`/`default`. |
| `lib/SQLite/library.json` | Ensure the VFS + store compile and link SQLite. |

---

## 6. Testing & verification

1. **Native unit tests (host, fast):** extend `test/` with a CMake target that backs the `hal` VFS with POSIX files, then asserts:
   - `GlobalReadingStats` round-trips through SQL (insert → reopen DB → load equals saved).
   - `BookReadingStats` round-trips per book id; `remove()` deletes the row.
   - `recordReadingSpan` into buckets/history matches CrossInk's existing expectations.
   - With DELETE journal, the `.db` is left self-contained (no `-wal`/`-shm`); `integrity_check` passes after an interrupted commit.
2. **Simulator smoke test:** `scripts/run_simulator_smoke_test.py` (x4-pro-simulator profile) exercises open-book → read → close → open stats activity, confirming no crash and that a row exists.
3. **Device build:** `pio run -e x4pro` must compile + link (the hard gate — SQLite + VFS + stats + font changes). Then `pio run -e x4pro -t upload` + Serial monitor to confirm `ReadingStatsStore::open()` succeeds and the DB initializes on a real SD card.
4. **Manual on-device:** open a book, read past the dwell threshold, close, reopen the Book Stats activity → see time/pages; open Global Stats → see totals/streak. Disable tracking in settings → no rows written.

---

## 7. Risks & open questions

- **R1 (highest):** the custom VFS locking semantics under DELETE journal. Mitigated by the single-connection invariant (one `sqlite3*`, main/reader task only); validate with `PRAGMA integrity_check` after recovery and via host `test/` round-trips **before** device flashing.
- **R2:** SQLite RAM footprint on x4pro. Mitigation: `cache_size` capped (~-128 ⇒ 512 KB), 4 KB pages, PSRAM allocation; measure heap via Serial. Confirm Sqlite3Esp32 routes allocations to PSRAM under `BOARD_HAS_PSRAM`.
- **R3 (resolved, C4):** `book_id` = full cache-path string (e.g. `epub->getCachePath()`), **not** the bare `std::hash` at `EpubReaderActivity.cpp:125`.
- **R4 (resolved):** `clockUtcOffsetQ` (`CrossPointSettings.h:216`, uint8_t, 48=UTC+0, quarter-hour bias) matches CrossInk's expectation; port-friendly.
- **R5 (resolved):** `FreeInkApp.h` exists in this freeink-sdk pin; font-downloader divergence is UI-base drift only, and D5 is now a verify-only pass (C1).
- **R6 (new, blocking):** `HalFile` needs `truncate()` + `sync()` added (D3b) before the VFS can checkpoint WAL.

---

## 8. OpenCode review outcome (resolved)

Reviewed by OpenCode (non-interactive `run`, repo-grounded). Ranked findings incorporated above:
1. **C1 (blocking, fixed):** D5 was a stale premise — font downloader already done in CrossPoint; now a verify-only pass.
2. **C2 (blocking, fixed):** gate on `BOARD_HAS_PSRAM`, not a device list (excludes `sticky`).
3. **VFS gaps (blocking, fixed):** `HalFile::truncate()` + `HalFile::sync()` added to §5 + D3b.
4. **C3 (fixed):** replaced `AppCapabilities` references with `BoardConfig`/macros.
5. **VFS atomicity + `xDeviceCharacteristics`** (no ATOMIC/POWERSAFE) + single-connection invariant documented.
6. **R2 (fixed):** concrete `cache_size` + PSRAM allocator noted.
7. **C4 (fixed):** `book_stats` keyed on full cache path, not bare `std::hash`.
8. **Schema (fixed):** `user_version`, integer bucket columns, UPSERT `saveGlobal()`.
9. **R4/R5 (resolved):** `clockUtcOffsetQ` and `FreeInkApp.h` confirmed.

**Net:** stats-subsystem plan (D1/D2/D3/§4/§5-stats) approved as architecturally sound; font-downloader half reworked to verify-only; gating + VFS primitives corrected. Ready to implement.
