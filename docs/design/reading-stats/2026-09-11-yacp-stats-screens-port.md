# YACP Reading-Stats Screens Port — Design

- **Date:** 2026-09-11
- **Status:** Proposed — awaiting user review
- **Scope:** `Belphemur/XPoint` (`feat/reading-stats-screens`, worktree `crosspoint-x-reader-rs-screens`)
- **Source analyzed:** `Sichroteph/YACP` @ `a96da86` (v1.6.2), cloned at `/home/balorworkspace/eink/YACP`

## 1. Background and source analysis

YACP (a CrossPoint fork) ships four reading-stats screens on top of the shared
CrossPoint stats core. Their reading-stats code is a direct descendant of the
same codebase we ported the card grid from (crossink), so the port is
structurally close — but YACP and our fork diverged in the meantime:

| Area | YACP v1.6.2 | Our fork (develop, v7/v5 record) |
|---|---|Design column |
|---|---|---|
| Book record | 73-byte "v5" record, legacy pace fields (bytes 12–15) live, **no WPM/Session windows** | 150-byte v7 with `WpmWindow` + `SessionWindow` (trimmed means), legacy pace fields retired to reserved |
| Global record | 159-byte v3, streak/history present, no windows | 225-byte v5 with windows |
| Stats UI entry | Single `BookStatsActivity` with **6 pages** (Summary, ReadingRhythm, FinishedBooks, Achievement, AllDevices, EditDates) | `BookStatsActivity` (single page, Clear-pace flow) + `GlobalStatsActivity` (single page, clear-global flow) |
| Daily history | Separate `DailyReadingHistory` class (own `daily_reading.bin`) | Reading-history bits already live **inside** global v5 record (bytes 71–162) |
| Completed books | `FinishedBooksIndex` (`/.crosspoint/finished_books.bin`, 32 entries, magic `CPFB`) | Only a `completedBooks` counter |
| Achievement | Reading Achievement screen on completion | None |
| Date editing | `EditDates` page | None |

YACP's `BookStatsActivity` is a heap-allocated multi-page activity holding
`BookReadingStats` + two `GlobalReadingStats` snapshots + a `DailyReadingHistory`
(730 bytes) + a `std::vector<FinishedBookEntry>` (32 × ~300 B). Rendering goes
through `BookStatsView` free functions; pages are switched by the
shortcut-button model (Up/Left = previous, Down/Right = next), with
Confirm = Edit-dates (book page) or exit, Back = exit.

### 1.1 The four screens (from README screenshots + code)

**Reading Stats summary (combined).** Two cards: the current book card
(title, sessions / reading time / progress, avg session / time left / reading
speed, started + est. finish) and the global card (sessions / reading time /
reading speed, avg session / reading streak / books read). YACP combines both
on one screen; our fork has them as two separate activities.

**Reading Rhythm.** Three section cards:
1. *Recent Activity* — 7×~15 grid of dots (one row per weekday, one column per
   ~day) over the last ~3.5 months, dot size/dither by minutes; legend
   "Minutes ◦1 ●15 ●30+" and "Today" ring.
2. *Weekly Reading Time* — ~12 bars, dashed average line, "Avg. 4h 37 min" +
   "This week 1h 17 min" footer.
3. *Reading Days by Month — 12 mos.* — monthly bar chart with day-count
   labels above each bar, "Total 211 days" / "Avg./month 18 days" footer.
   The monthly chart drops itself when vertical space is tight (compact
   layout check `availableH >= fullContentH + 12`).

**Reading Achievement.** Celebratory check-circle graphic, "Book finished —
well done!", title — author, a 3×2 cell grid (Reading Time / Sessions /
Avg Session, Favorite Time / days / Books Finished), the reading span
("Jul 5 to Jul 28 · 24 days"), and a Total Reading Time card.

**Finished Books.** Total Reading Time card, then month-grouped entries
(4 per page): title, start→finish dates with a connecting line, duration;
"Back/Next" pagination; supports ~8 pages for 32 entries.

### 1.2 YACP's page navigation model

```
Summary ──(More/Down)──► Reading Rhythm ──(More)──► Finished Books ──(More)──► (All Devices)
   ▲                        ▲  (Prev/Up = Summary)          ▲ (Prev = Rhythm)        ▲
   └────────────────────────┴──────────────────────────────┴────────────────────────┘
Confirm on Summary = EditDates; on other pages = exit; Back = exit from anywhere.
```

## 2. Design: what we port and how

### 2.1 Strategy

The port is UI-first, persistence-light. Our fork's v7/v5 records already
contain everything except the finished-books index; YACP's rhythm data
(per-day minutes) is derivable from our existing global record (bitfield =
read day, `timeOfDaySeconds`/`dayOfWeekSeconds` already persisted). We port
the screens and wire a new **sub-menu**; we do NOT port YACP's `DailyReadingHistory`
separate file (our global record already carries the 730-day bitfield) nor
their 73-byte book format (we keep v7).

YACP's `StatsBackup`, `NearbyStatsSync`, no-RTC single-screen layout, and
simulator demo are **out of scope** (orthogonal features; our fork has its own
settings/backup flows).

### 2.2 Menu structure — Reading Stats main menu with sub-menu

The user's directive: make the Reading Stats main menu entry have a **sub-menu**
to see the different screens. We replace the current direct-push model:

```
Home ▸ Reading Stats  →  ReadingStatsMenuActivity (UiListActivity)
                          ├─ This Book (or "Reading Stats" combined page)
                          ├─ This Device (existing GlobalStatsActivity page)
                          ├─ Reading Rhythm
                          ├─ Finished Books
                          └─ Edit Dates / Achievement (contextual, book only)
```

Design decisions:

- **A dedicated `ReadingStatsMenuActivity`** (new, `src/activities/reader/` or
  `src/activities/settings/`), a `UiListActivity` with 3–5 rows depending on
  context. From Home (no book context) the menu shows: This Device, Reading
  Rhythm, Finished Books. From the reader (book context) it shows: This Book,
  This Device, Reading Rhythm, Finished Books. This is the single entry point
  replacing `goToGlobalStats()` and the reader's stats toolbar button.
- **One activity per screen, not YACP's single multi-page activity.** Rather
  than port YACP's 6-page `BookStatsActivity` (which conflates per-book,
  sub-menu, and completion flows and would need a rewrite of our Clear-pace
  flow), each screen stays its own activity. Rationale: our fork's
  `BookStatsActivity`/`GlobalStatsActivity` already have distinct
  `ActivityResult` clear-pace contracts with the reader; a multi-page activity
  would need to multiplex those contracts by page. KISS: the sub-menu is the
  pager.
- **`ReadingRhythmActivity`** (new): renders the YACP rhythm page from
  global v5 data (bitfield → dots; weekly bars from a rolling 12-week
  minutes array computed from the bitfield × avg; monthly bar chart from the
  bitfield). See §2.5 for the data gap.
- **`FinishedBooksActivity`** (new): renders the finished-books list from the
  new `FinishedBooksIndex`.
- **`BookStatsActivity` gains pages** — the simplest structure that satisfies
  the achievement requirement: it keeps its single card-grid page and gains
  `EditDates` and `Achievement` pages (YACP's `renderEditBookDatesPage` /
  `renderBookDatesPage` functions port nearly verbatim). The existing
  Clear-pace Confirm contract stays on the main page.
- **Home menu entry** (`STR_READING_STATS`) now pushes
  `ReadingStatsMenuActivity` instead of `goToGlobalStats()`.
- **Reader toolbar** stats button pushes the menu with book context.
- **Achievement trigger:** port YACP's completion flow (auto-complete on real
  end + 100% confirm prompt + pending-achievement flag) so finishing a book
  pushes `BookStatsActivity` with `InitialPage::Achievement`. This is the one
  flow that must touch the reader; it is a separate commit but the same PR.

### 2.3 New persistence: `FinishedBooksIndex` (port near-verbatim)

YACP's `FinishedBooksIndex.{h,cpp}` ports with minimal changes:

- Same on-disk format (magic `CPFB`, version 3, ≤32 entries, FNV-1a path key,
  title ≤160 B / author ≤120 B, atomic tmp → verify → `.bak` rotation) —
  matches our fork's existing atomic-write conventions.
- Dependencies exist on our fork: `HalStorage`, `Serialization.h`, `FsHelpers`,
  `lib/Xtc`, `Epub` — all present.
- The migration source ("recover completed entries from the recent-books
  list") ports too: seed from `RecentBooksStore` entries whose book stats say
  `isCompleted` (our fork records `isCompleted` already; the recent-books
  store holds path/title/author).
- New file `src/activities/reader/FinishedBooksIndex.{h,cpp}`.
- **Record migration is unaffected**: `finished_books.bin` is a new
  standalone file, no v7/v5 bump.

### 2.4 Persistence unaffected: v7/v5 records untouched

No record bumps. Everything the screens display comes from the current
records:

- Summary cards: unchanged (already ported, PR #52/#54).
- Rhythm: global v5 bytes 71–162 (730-day bitfield) for read/not-read days;
  `timeOfDaySeconds`/`dayOfWeekSeconds` for the existing card-grid charts.

### 2.5 Reading Rhythm data gap — minutes-per-day

YACP's Recent Activity dot intensity + weekly chart use **minutes per day**;
our v5 record stores only a read/not-read **bitfield**. Options:

- **Option A (chosen):** derive per-day minutes from the bitfield, rendered
  at two intensity levels only (read / not read) — YACP's dot-size ladder
  (1/15/30+ minutes) needs per-day minutes we don't have. Weekly Reading
  Time bars need per-week minutes: we can approximate weekly minutes from
  `totalReadingSeconds` over... no — the totals are all-time, not per-week.
  Weekly bars need real data.
- **Option B:** bump global v5→v6, adding a 91-day `uint16` minutes array
  (182 B, record 225→407 B) alongside the bitfield; rhythm charts render
  real minutes. Backward/forward migration follows the standard in-place
  rules (v5 loads as v6 with bitfield-only rendering; v6 files load on older
  builds → the destructive-save guard).

**Decision required from user:** intensity levels vs record bump.
**Recommendation: Option B** — the rhythm screen is the centerpiece of the
port, and two-level dots would look visibly degraded next to YACP's
screenshot. The bump is mechanical (per-version decode constants, one new
array at bytes 225–406, doc §3, wire-offset tests) and the destructive-save
guard already handles future-format detection.

### 2.6 Files touched

| File | Change |
|---|---|
| `src/activities/reader/FinishedBooksIndex.{h,cpp}` | **new** — port |
| `src/activities/reader/ReadingRhythmActivity.{h,cpp}` | **new** — rhythm screen |
| `src/activities/reader/FinishedBooksActivity.{h,cpp}` | **new** — finished-books list screen |
| `src/activities/reader/ReadingStatsMenuActivity.{h,cpp} ` | **new** — sub-menu |
| `src/activities/reader/BookStatsView.{h,cpp}` | add `renderReadingRhythmPage`, `renderFinishedBooksPage`, `renderEditBookDatesPage`, `renderReadingAchievementPage` (ports) |
| `src/activities/reader/BookStatsActivity.{h,cpp}` | add EditDates + Achievement pages, InitialPage param |
| `BookReadingStats.h` | add `completionAchievementPending` + `completionPromptDismissedAtHundred` flags — **requires v7→v8 bump (2 bits)** |
| `GlobalStatsActivity` | rendered page becomes the "This Device" page of the menu |
| `HomeActivity::onReadingStatsOpen` | push the sub-menu |
| `EpubReaderActivity` | stats toolbar → menu; port completion flow (`setBookCompleted`, 100% prompt, achievement push) |
| `XtcReaderActivity` | same completion flow |
| `english.yaml` + regenerated `I18nKeys.h` | ~25 new strings |
| `test/reading_stats/` | FinishedBooksIndex round-trip/migration tests; rhythm data derivation tests; wire-offset pins for v8 |
### 2.6 The two record bumps (conditional on the answers to §3)

- **Global v5→v6** (only if Option B, §3 Q1): append a 91-day `uint16`
  minutes array (182 B, record 225→407 B) after the session window. One-hop
  in-place migration; v5 loads as v6 with bitfield-only rendering; per-version
  decode constants mandatory; wire-offset tests + doc §3 in the same commit.
- **Book v7→v8** (only if ride-along, §3 Q2): pack the two completion-flow
  flags (`completionAchievementPending`, `completionPromptDismissedAtHundred`)
  into one reserved byte at 134 (record stays 135 B). Without them the
  achievement flow can't survive a reboot mid-flow.

### 2.7 Risks / constraints

- RAM: `FinishedBooksActivity` holding 32 entries ≈ 10 KB heap — fine on all
  boards (C3 has ~380 KB DRAM; activity is heap-allocated and deleted on exit).
  The Rhythm activity holds only global snapshot + derived arrays (~1 KB).
- YACP's no-RTC single-screen layout is out of scope; our fork always has RTC
  clock handling for the stats screens already.
- YACP's `DailyReadingHistory` separate-file design is rejected (our global
  record already has the bitfield; a second file duplicates state and the
  nearby-sync contract differs).
- Screenshots-driven layout: the four screens' geometry comes from YACP's
  `kDefaultLayout`/`kCompactLayout` constants — port the helpers verbatim
  (`drawSectionCard`, `drawHorizontalBars`, `drawStatCell`) and adjust for our
  fork's `BookStatsView` namespace + `formatStatCell` conventions.
- **Co-author trailer**: YACP is *not* crossink — `Julia Nguyen` trailer is
  NOT warranted for this port (user rule: direct crossink ports only).
- i18n: all new strings via `tr(STR_*)`, printf-style formats only.
- `requestUpdate()` in every new activity's `onEnter()` (blank-screen pitfall).

## 3. Open questions for the user

1. **Rhythm data:** Option A (bitfield-only, 2 intensity levels, no bump) or
   Option B (global v5→v6 bump with 91-day minutes array, real minutes)? Recommend B.
2. **Book-record v8 for the two completion flags:** ride along in this PR
   (single PR, two bumps) or a follow-up PR (achievement flow lands without
   the flags first, flags+flow in a second PR)? Recommend ride-along.
3. **Menu structure:** as drafted (This Book / This Device / Reading Rhythm /
   Finished Books; reader adds Edit Dates and Achievement context) — or a
   flatter structure?
4. **Upstream-worthiness:** keep fork-only, or plan to upstream this (would
   change how aggressively we bump records)?

## 4. Implementation plan (post-approval)

Ordered tasks for the pi implementation run:

1. `FinishedBooksIndex` port + host tests (round-trip, recover-from-recent, migration from legacy).
2. `BookStatsView` rhythm/finished/achievement/edit-dates renderers (port from YACP, adapt to our helpers).
3. `ReadingRhythmActivity` + `FinishedBooksActivity` + `ReadingStatsMenuActivity` + wiring (Home, reader toolbar).
4. Completion flow + achievement (v7→v8 flags if ride-along) + tests.
5. Full gate: host tests (56/56 baseline + new), `pio run -e x4pro` fresh build + `default`/`sticky`/`papermono`, clang-format, cppcheck.
6. PR with `feat(reading-stats): ...`, review-thread triage loop, squash merge.

## 5. Sources

- YACP clone: `/home/balor/workspace/eink/YACP` @ a96da86.
- Screens: `docs/images/yacp/media/{reading-stats,reading-rhythm,reading-achievement,finished-books}.png`.
- Our design docs: `docs/design/reading-stats/` (esp. `2026-09-10-avg-session-trimmed-window.md` for bump mechanics).
- Skill: `crosspoint-reading-stats` (hard rules §Hard rules apply throughout).
