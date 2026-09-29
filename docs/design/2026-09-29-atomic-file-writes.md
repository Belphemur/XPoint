# Consolidating CrossPoint file writes onto the SDK's atomic write path

Date: 2026-09-29
Status: implemented (this commit)

## 1. SDK contract — already pinned, no submodule bump

`Belphemur/freeink-sdk` commit `2ab0b6f85b1fe96a7040d06193ab486db58b7115`
("Make SD card writes atomic using temp file swap") changed
`SDCardManager::writeFile` to a temp-swap contract:

1. writes to `path + ".tmp"`,
2. requires `f.print(content) == content.length()` (rejects short writes),
3. requires `f.close()` to succeed,
4. only then publishes via `replaceFile(tmp, path)`,
5. removes the `.tmp` on any failure so the previous file survives.

Evidence (`freeink-sdk` submodule, pinned at `1bc845cc`):

- `merge-base --is-ancestor 2ab0b6f8 1bc845c` → true, so the change is already
  in the pinned digest. **No submodule pointer bump is part of this task.**
- `libs/hardware/SDCardManager/src/SDCardManager.cpp:390` — atomic `writeFile`
  body present in the worktree.
- `libs/hardware/SDCardManager/include/SDCardManager.h:60-67` — `replaceFile`
  is a **public inline member** of `class SDCardManager`
  (`(!vol().exists(path) || vol().remove(path)) && vol().rename(tmpPath, path)`),
  so it is directly reachable from firmware. No SDK change is required or made.

The gap was in the CrossPoint HAL: `HalStorage` exposed `writeFile` /
`exists` / `remove` / `rename` but not `replaceFile`, so every binary-write
site re-implemented the FAT rename-requires-empty-destination dance by hand
(`grep -rn replaceFile lib/ src/ test/` returned nothing before this change).

### HAL helper (new)

`HalStorage::replaceFile(tmpPath, path)` (`lib/hal/HalStorage.h`,
`lib/hal/HalStorage.cpp`) delegates to the SDK's public
`SDCardManager::replaceFile` under `HAL_STORAGE_WRAPPED_CALL`, so the
`storageMutex` is held for the entire `exists → remove → rename` sequence —
an unserialized multi-call dance from application code would be the exact
SdFat race the mutex exists to prevent.

## 2. Inventory and migration decisions

Every site that publishes a fully written temp file over a live destination.
Class **A** = plain remove+rename (what `replaceFile` does). Class **B** =
adds a stronger guarantee the SDK primitive does not provide.

| # | Site | Guarantee | Class | Decision |
|---|------|-----------|-------|----------|
| 1 | `src/activities/reader/ProgressFile.h:32` `writeAtomic()` — `progress.bin` | tmp → remove(final) → rename. Comment (:21-33) documents "crash-safe, not metadata-atomic". | A | Migrated: tail is now `Storage.replaceFile` |
| 2 | `src/BookFontLoader.cpp:505` `BookFontLoader_writeFingerprintCache()` — 28-byte fingerprint record | stage `.tmp` → `exists && remove` → `rename`; removes temp on every failure. | A | Migrated: `exists && remove` + `rename` collapsed into `Storage.replaceFile` |
| 3 | `src/main.cpp:331` `saveSleepFrameBuffer()` — `sleep_frame.bin` | **See Finding 1**: dropped the old frame before writing the temp. | A+ | Reordered (temp written first), publish via `Storage.replaceFile` |
| 4 | `src/activities/reader/GlobalReadingStats.cpp:285` `saveToFile()` — `global_stats.bin` | tmp → size-verify → rotate to `.bak` → remove stale `.bak` → rename; restores `.bak` if final rename fails. | B | Tail publish step switched to `Storage.replaceFile`; rotation + restore untouched |
| 5 | `src/activities/reader/FinishedBooksIndex.cpp:94` — `finished_books.bin` | tmp → read-back verification → pre-restore corrupt primary from `.bak` → rotate → publish → restore on failure. | B | Tail publish step switched to `Storage.replaceFile`; rotation, verification, and restore untouched |
| 6 | `src/adapters/SdCardCacheStorage.cpp:92` `beginWrite`/`endWrite` — TTF/FIBP page cache | tmp → `sync()` → rotate final→`.old` → rename → remove `.old`; `.old` recovery in `beginWrite`; `writeFailed_`/`endWriteFailed_` flag ordering is load-bearing for `PageCacheWriter` cleanup. | B (strongest) | **Not migrated.** The rotate-aside (rename, not remove) before publish is required to retain the previous good cache across a crash between the two renames — `replaceFile` removes, not rotates, and would regress that guarantee; see §5.1 |
| 7 | `src/network/WebDAVHandler.cpp:95` `RAW_END` publish — WebDAV `PUT` | streams to `.davtmp`, then `Storage.remove(_putPath)` + `HalFile::rename()` on the open handle. | A | Migrated: publish is `Storage.replaceFile` on closed temp + final |

Deliberately **not** treated as atomic-write sites (they are scratch/staging
files, truncated per use and reopened for read, never published over a live
file): `src/util/DictHtmlPages.cpp:19` (`dicthtml.tmp`),
`src/util/Dictionary.cpp:19` (`dict.tmp`). Left as-is on purpose.

## 3. Finding 1 — `saveSleepFrameBuffer` destroyed the good frame first

Old order (`src/main.cpp` around 365-390): (1)
`exists(final) && remove(final)` — deleted the good file before the new one
existed; (2) write `sleep_frame.bin.tmp` (framebuffer + 4-byte Adler-32
trailer), flush/sync/close; (3) rename tmp → final. A power cut between (1)
and (3) left **neither** file: the device wakes to no-frame sleep. Every other
site writes the temp first and only then touches the destination, bounding the
loss to "previous good file survives".

The old comment also documented a deliberate semantic choice: on failure the
old frame is dropped rather than kept, because the panel already shows the new
sleep screen, so a restored old frame would no longer match what is displayed
(the file is the baseline of what is on the panel — the quick-resume
contract). Reordering is therefore a real behaviour change, not a pure
refactor.

**Resolution.** The write is reordered so the temp is fully written and
verified (bytes + trailer + sync + close) **before** the destination is
touched; publication is a single `Storage.replaceFile(tmp, final)` call. Two
consequences, both intended:

- The "crash between remove and rename" window can no longer eat both files at
  the write level: before any destination mutation the temp is complete, so a
  crash in that window leaves the complete tmp on disk next to the old frame.
- The documented drop-old-on-publish-failure policy is kept: if the final
  publish fails, the old frame is NOT restored to the final path. `replaceFile`
  returns false after `remove(final)` succeeded but `rename` failed — old
  frame gone, complete `.tmp` left behind — and the code then removes that
  `.tmp`. A missing frame just restores no-frame sleep, which stays the safe
  direction (never show a frame that mismatches the panel). What changed is
  only the front of the sequence: the failure that used to destroy both files
  (a power cut mid-temp-write) now costs nothing.

The comment block over the write describes the merged state.

## 4. Finding 2 — per-book stats had no atomicity and no backup

`BookReadingStats::save()` (`src/activities/reader/BookReadingStats.cpp:271`)
was a truncate-in-place write of the 135-byte per-book record — the one record
class `GlobalReadingStats` protects with tmp → size-verify → `.bak` rotation
and restore-on-failure. The two halves of the same subsystem had opposite
durability. A torn write did not leave garbage (the loader rejects short or
version-mismatched records through the size/version check) — it left *no*
record at all: the entire per-book history (session count, daily reading
seconds, WPM window, start/finish dates) destroyed by one interrupted write,
with the recovery path being "start fresh".

**Resolution.** The save now writes to `stats_vN.bin.tmp` (full record, single
sequential write, `flush` + `sync` + `close` all checked) and publishes with
`Storage.replaceFile`, so the on-disk record is either the complete old record
or the complete new record — never a torn middle. A short write or failed
sync/close removes the temp and leaves the previous record intact, which is
what the old "torn writes self-heal" comment actually guaranteed but with
survivability instead of destruction. The one-time legacy migration (deleting
`stats_v5/6/7.bin` after a confirmed v8 write) moves after the verified
publish, so legacy files are only dropped once the new record is durably on
disk — preserving the invariant the short-write path already defended.

**No `.bak` for per-book records, and why that is defensible against
hard-rule #1.** Hard-rule #1 ("a short write followed by a legacy delete is
unrecoverable data loss") is satisfied by the reorder itself: the legacy
deletes now run only after a fully verified atomic publish, so the short-write
+ legacy-delete unrecoverability scenario cannot occur. The residual
publish-failure mode is: temp verified, old record removed, rename fails —
leaving no record. For the global record that justified the `.bak`: global
history is a single file at a fixed path, accumulated over the life of the
device, not regenerable from anything. Per-book records are strictly weaker
data: they live in the EPUB's per-hash cache dir, whose lifecycle is owned by
the cache (deleting `.crosspoint/epub_<hash>/`, moving or renaming the book, a
render-settings change that invalidates the layout cache, or a manual cache
clear) removes them as a *design* consequence and restarts collection at zero
— per-book history is already non-durable by design, and the reader's own
opens regenerate entry records. Weighing one extra dir-op pair (≈4 SD wins)
and a stale-`.bak` recovery branch against protecting data the subsystem
already treats as cache-lifetime, we chose atomic-publish without `.bak`,
matching `ProgressFile`'s precedent for the identical volatility class
(per-dir record, same rename-publish tail) and strictly improving on the old
truncate-in-place behaviour. If this is ever revisited, the trigger should be
telemetry showing real-world failed renames, not speculation.

**No version bump.** The record layout, `STATS_FILE_VERSION` (v8, 135 B), and
`statsFileNameForVersion` naming are unchanged — this changes durability of
the write path, not the on-disk format. Loader compatibility tests are
untouched. (`docs/design/reading-stats/2026-08-25-binary-files.md` §2 is
amended in this commit to state the per-book save is now the same atomic
tmp+publish shape the global save uses.)

## 5. Class-B sites kept

### 5.1 `SdCardCacheStorage` — not migrated

The `.old` rotation (rename, not remove) is the documented "mid-build failure
retains previous final" contract; `beginWrite`'s `.old` recovery and the
`writeFailed_` / `endWriteFailed_` ordering exist so that the
`PageCacheWriter` failure-cleanup `remove()` cannot delete a good cache.
`replaceFile` performs `remove(final)` first — swapping the third-op rename
for that remove+rename pair would (a) reintroduce the both-files-gone window
the rotation exists to avoid and (b) interact with two flags whose ordering
has already been tuned against host tests exercising exactly these paths.
LRU-rotating two renames for a `remove` + `rename` with identical observable
behaviour in the success path buys nothing and risks a subtle regression in
live code. Left entirely alone.

### 5.2 `GlobalReadingStats` / `FinishedBooksIndex` — publish tail only

Both keep their full guarantee chain (temp staging, verification, `.bak` /
backup rotation, restore-on-failure, pre-rotation recovery of a corrupt
primary). Only the final tmp→final publish replaces the hand-rolled
`exists && remove` pair with `Storage.replaceFile`, consolidating the dance
without flattening the extra guarantee. The per-site comments explaining
*SdFat* rename O_EXCL semantics (`FatFile::rename fails when the destination
exists`) move to failure handling of `replaceFile` at those sites; the
backup-rotation renames keep their explicit remove-then-rename (they rotate a
live file aside rather than replacing it, so `replaceFile` is not the right
primitive for them anyway).

## 6. Doc updates shipped with the code

- `lib/hal/HalStorage.h` — `writeFile` comment no longer says "Overwrites
  existing file"; it now states the atomic temp-swap contract (`a fully
  written staging copy is swapped in only after the whole payload is written;
  the previous file survives any failure`). The stale text contradicted the
  SDK the comment wraps.
- `src/activities/reader/BookReadingStats.cpp` — the "torn writes self-heal…
  short writes leave a valid legacy file" comment described integrity-only
  recovery of a truncate-in-place write; it is replaced by the atomic-replace
  comment above. The global path's precedent in the same subsystem is doing
  tmp+verify+bak for a 407-byte record; the per-book 135-byte record no longer
  has *worse* durability than the record derived from it.
- `docs/design/reading-stats/2026-08-25-binary-files.md` — persistence-layer
  truth for the reading-stats contract updated for the per-book save.
- Host-test `HalStorage` stubs gain `replaceFile` with **FAT O_EXCL
  semantics**: a `rename` onto an existing destination must fail, so tests
  keep modelling the device instead of POSIX `std::rename`'s silent replace.
  Existing in-memory stubs already had the destination-exists failure for
  `rename`; `std::rename`-based stubs were given the explicit guard plus the
  pre-remove.

## 7. Tests added

- `test/progress_manager/FatReplaceSemanticsTest` — pins the host-stub
  `replaceFile`'s FAT O_EXCL behaviour: rename onto an existing destination
  fails and preserves both inputs; rename onto a missing destination succeeds;
  failure injection removes the temp and leaves the previous final intact.
- `test/reading_stats/BookStatsTempFailureTest` — pins per-book durability: a
  save that fails mid-write (short write / failed sync / failed close) leaves
  the previous on-disk record byte-identical and loads it back correctly.
