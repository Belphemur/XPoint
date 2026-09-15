# Home Button Unification

- **Status:** In progress
- **Date:** 2026-09-15
- **Scope:** Replace the fork's parallel home-key action catalogs and gesture engines with upstream
  `HomeButtonAction` from `32f2b2aa25531a9a419097b066ae86e4d22dfd36`, while preserving every
  fork-only action and behavioral guard.
- **Owner directive:** "We should use the new namings of the upstream, but be sure we don't lose
  features that we created — so this will simplify following merges from upstream."

## 1. Goal and success criteria

The fork independently shipped a capacitive-home-key remap before upstream shipped
`feat: add home button shortcuts (#3516)`. The `-s ours` sync merge recorded upstream's commit in
history but deliberately kept the fork tree. That leaves two catalogs for the same user knowledge
and two tap/hold state machines in the tree.

Success means:

1. Upstream's `HomeButtonAction` values `0..10`, file names, class names, settings keys, and wiring
   points are adopted byte-for-byte where they exist.
2. Fork-only actions and behavioral guards survive by appending and by relocating into the adopted
   structure.
3. The fork's duplicate catalog (`HOME_ACT_*` + `LP_MENU_*`) and duplicate gesture machinery
   (`HomeTapTracker`, `deferredHomeGesture`) are deleted, not retained behind a compatibility layer.
4. Existing `settings.json` keeps its meaning after a one-time migration and resave.
5. Both firmware feature classes build: `FREEINK_CAP_HOME_KEY` (X4 Pro) and
   `FREEINK_CAP_MENU_BUTTON` (X4/X3/X4 Classic/Sticky).

The worktree was fast-forwarded from the brief's starting `1996f070` to current `origin/develop`
`86ab2c42` before implementation so the native-TTF reader and PlatformIO tooling fixes are the base
for adaptation. This was a `--ff-only` operation; no campaign work was rewritten.

## 2. Audit: current divergence

All line numbers are on branch `feat/home-button-unification` at `2bd70080`.

| Fork area | Current location | Problem under unification |
| --- | --- | --- |
| Fork catalog enum | `src/CrossPointSettings.h:167-180` (`HOME_ACT_*`, `OFF=0..GO_BACK=6`) | Different persisted values than upstream |
| Confirm-hold catalog | `src/CrossPointSettings.h:187-193`, `:348` (`LP_MENU_*`, `longPressMenuFunction`) | Second action catalog for reader holds |
| Home action fields | `src/CrossPointSettings.h:417-420` (home-key-gated; double field uses `Double`Click) | Upstream uses ungated fields and `homeButtonDoubleTapAction` |
| Settings list builders | `src/SettingsList.h:201-210` (`buildLongPressMenuValues`, `buildHomeButtonValues`) | Three ad-hoc enum rows duplicate the future `home_button::ACTION_LABELS` |
| Settings list rows | `src/SettingsList.h:362-367` and `:379-385` | Four rows describe one Home/hold control surface |
| Home dispatcher | `src/main.cpp:253-304` (`toggleFrontlightByShortcut`, `executeHomeButtonAction`) | Uses fork enum, not upstream catalog |
| Tap state machine | `src/main.cpp:51`, `:93`, `:336-404`; `src/util/HomeTapTracker.h` | Parallel to upstream `HomeButtonInput`; 300 ms window differs from upstream 350 ms |
| Main-loop arbiter call | `src/main.cpp:1128-1137` | Separate arbitration before `ActivityManager::loop()` |
| Gesture latch | `src/MappedInputManager.h:90-98`, `:152-156`; `src/MappedInputManager.cpp:287-299` | `deferredHomeGesture` + `wasHomeKeyHold` duplicate classifier output |
| Reader hold consumer | `src/activities/reader/EpubReaderActivity.cpp:1160-1190` | Reads `LP_MENU_*` |
| Reader hold threshold | `src/activities/reader/EpubReaderActivity.cpp:1610-1620` | Reads `LP_MENU_*` |
| Reader gesture comment | `src/activities/reader/ReaderUtils.h:139-146` | Describes legacy setting |
| Page-turn action | `src/activities/reader/ReaderUtils.h:51-69` | No `NextPage` arm |
| Blocking transfer loops | `src/activities/browser/OpdsBookBrowserActivity.cpp:530-537`; `src/activities/settings/FontDownloadActivity.cpp:599-609`; `src/activities/network/CrossPointWebServerActivity.cpp:383-397` | Consume raw Home and lose other configured actions |
| Web settings enum source | `src/network/CrossPointWebServer.cpp:1205`, `:1287` | Reads `std::vector` only; cannot host a static enum |
| Host tests | `test/home_tap_tracker/`, wired at `test/CMakeLists.txt:43` | Test the classifier to be deleted |

Upstream `32f2b2aa` supplies the adopted core:

| Upstream path | Adopted role |
| --- | --- |
| `src/util/HomeButtonInput.h` | `HomeButtonAction` catalog + pure gesture classifier |
| `src/HomeButtonSettings.h` | `home_button` labels, member pointers, persisted keys, `isSetting()` |
| `src/MappedInputManager.{h,cpp}` | One classifier per input frame, deferral, Confirm aliasing, held-time neutralization |
| `src/activities/ActivityManager.cpp` | Activity transitions reset stale classifier state |
| `src/main.cpp` | `toggleFrontlight()` and global ToggleFrontlight/Refresh dispatch |
| `src/activities/reader/*` | Reader actions and `NextPage` arm |
| `src/activities/settings/{SettingsActivity,HomeButtonSettingsActivity}.*` | Dedicated Home Button screen and `StaticEnum` |
| `src/CrossPointSettings.{h,cpp}` | Persisted fields/keys, legacy hold migration, static-enum clamp |
| `src/network/CrossPointWebServer.cpp` | Web API uses the same labels and clamp |
| `test/home_button/` | Classifier tests |

## 3. Board-capability matrix

`scripts/board_features_dump.c` derives capabilities from SDK `BoardConfig`:
`menu_button = dedicated Confirm pin || synthesized Confirm`.

| PlatformIO env family | `FREEINK_CAP_HOME_KEY` | `FREEINK_CAP_MENU_BUTTON` | Consequence |
| --- | --- | --- | --- |
| `default` (X4 + X3) | 0 | 1 | No Home key; Confirm hold must survive |
| `sticky` | 0 | 1 | No Home key; Confirm hold must survive |
| `x4c` | 0 | 1 | No Home key; Confirm hold must survive |
| `x4pro` | 1 | 0 | Home key; no dedicated Confirm hold |
| `papermono` | 0 | 0 | Neither surface |

This disproves using upstream's migration gate verbatim
(`BoardConfig::hasHomeKey()`): it would skip the `longPressMenuFunction` migration on exactly the
boards where that setting was exposed and used. The fork-specific gate is therefore
`FREEINK_CAP_MENU_BUTTON` for the legacy Confirm-hold value. The Home-key fields are made ungated so
one catalog and one Confirm-hold reader exist on every firmware feature class.

## 4. Unified catalog

Upstream values remain immutable; fork actions are appended:

| `HomeButtonAction` | Value | Source | Dispatch owner |
| --- | ---: | --- | --- |
| `Home` | 0 | upstream | `ActivityManager::loop()` through `wasHomeGesture()` |
| `Ignore` | 1 | upstream | classifier returns no dispatch |
| `NextPage` | 2 | upstream | reader via `ReaderUtils::detectPageTurn()` |
| `Refresh` | 3 | upstream | `main.cpp` global loop |
| `Footnotes` | 4 | upstream | reader |
| `Confirm` | 5 | upstream | `MappedInputManager` aliases to logical Confirm |
| `Sync` | 6 | upstream | reader |
| `Bookmark` | 7 | upstream | reader |
| `Dictionary` | 8 | upstream | reader |
| `ReaderMenu` | 9 | upstream | reader; non-reader remains a no-op unless a future activity implements it |
| `ToggleFrontlight` | 10 | upstream | `main.cpp` global loop |
| `Sleep` | 11 | fork | `main.cpp` global loop |
| `Screenshot` | 12 | fork | `main.cpp` global loop |
| `GoBack` | 13 | fork | `main.cpp` global loop; `handleBackOnCurrent()` then `popActivity()` |
| `Count` | 14 | boundary | label/static-assert count only |

A second frame-level gesture result is an additive fork extension to the otherwise upstream
classifier: `HomeButtonInput` records whether the returned action came from the single-tap expiry,
a double tap, or a hold. This is required to enforce the fork's old invariant that a **single tap**
is suppressed on the Home screen while double-tap and hold actions remain available there. It does
not change any upstream return value or state transition.

## 5. Persisted settings migration

`CrossPointSettings::fromJson()` performs the migration before `requestResave()`; it never saves
inside the store mutex.

### 5.1 Legacy fork `HOME_ACT_*` value map

Applies to values `0..6` loaded from old fork-written home fields. Because the unified catalog also
uses `0..6`, the old double-click JSON key (`homeButtonDoubleClickAction`) is the legacy marker: a
file carrying it was written by the fork's pre-unification firmware. On menu-button-only builds
with no home fields, `longPressMenuFunction` is the marker. This avoids remapping a fresh or
already-unified settings file on every load.

| Old `HOME_ACT_*` | Old value | New `HomeButtonAction` | New value |
| --- | ---: | --- | ---: |
| `OFF` | 0 | `Ignore` | 1 |
| `FRONTLIGHT` | 1 | `ToggleFrontlight` | 10 |
| `GO_HOME` | 2 | `Home` | 0 |
| `READER_MENU` | 3 | `ReaderMenu` | 9 |
| `SLEEP` | 4 | `Sleep` | 11 |
| `SCREENSHOT` | 5 | `Screenshot` | 12 |
| `GO_BACK` | 6 | `GoBack` | 13 |

### 5.2 Legacy key rename

Old double-click field/key: `homeButtonDoubleClickAction` (current `CrossPointSettings.h:419`,
`SettingsList.h:382-383`). New upstream field/key: `homeButtonDoubleTapAction`
(`git show 32f2b2aa:src/CrossPointSettings.h`, `src/HomeButtonSettings.h`). The old key is read only
when the new key is absent, and its value passes through the table in §5.1.

`homeButtonTapAction` and `homeButtonLongPressAction` keep the same JSON keys as upstream.

### 5.3 Legacy Confirm-hold map

Old `LP_MENU_*` values (`CrossPointSettings.h:188-192`) map to the unified long-press field:

| Old | Old value | New | New value |
| --- | ---: | --- | ---: |
| `KOSYNC` | 0 | `Sync` | 6 |
| `DISABLED` | 1 | `Ignore` | 1 |
| `BOOKMARK` | 2 | `Bookmark` | 7 |
| `DICTIONARY` | 3 | `Dictionary` | 8 |
| `READER_MENU` | 4 | `ReaderMenu` | 9 |

Gate this migration with `FREEINK_CAP_MENU_BUTTON`, not upstream's `BoardConfig::hasHomeKey()`.
The old row (`SettingsList.h:362-367`) was shown only on menu-button boards. Migration order:

1. If the new `homeButtonLongPressAction` key is present, map its `0..6` fork value.
2. Else, on a menu-button build, map `longPressMenuFunction`.
3. Delete `LP_MENU_*`, `longPressMenuFunction`, `buildLongPressMenuValues()`, and its Settings row
   in the same change.

## 6. Gesture model and fork invariants

`HomeButtonInput` uses upstream's 350 ms double-tap window, `wasHomeKeyPressed()` as second-contact
evidence, swipe cancellation, and unsigned millisecond-wrap arithmetic. Upstream's call order is:
swipe reset, hold, pending-window expiry, second-contact detection, tap/double classification.

The following fork behaviors are non-negotiable and remain in the adopted structure:

1. **Home-screen single-tap guard.** A tap-delivered action is not dispatched while
   `ActivityManager::isOnHomeScreen()` is true. Double-tap and hold deliveries still work.
2. **Zero-latency disabled gestures.** With tap and double-tap both `Ignore`, a tap produces no
   armed wait. A web/API settings change cannot expire a stale armed window into an action.
3. **Unconfigured hold is inert.** A hold mapped to `Ignore` performs no action.
4. **Stalled-loop recovery.** If expiry and the next physical tap occur on the same frame, the tap
   rearms a fresh window rather than being dropped.
5. **Power-button frontlight.** X4 Pro double-click frontlight remains, now gated by upstream's new
   `doubleClickPwrLight` toggle; `wasPowerConfirmClick()` waits for that enabled window.
6. **Blocking transfer deferral.** Transfer loops call `update(true)`. They retain immediate Home
   cancellation; all other actions are retained for the next normal main-loop frame.
7. **Activity transitions clear state.** `pushActivity()`, `popActivity()`, and
   `replaceActivity()` call `resetHomeButtonInput()`, as do reader overlay open/close.
8. **Physical Confirm hold remains available** on menu-button boards. It reads the unified
   long-press field: Bookmark/Dictionary use the hold threshold, Sync uses its threshold only with
   credentials, and ReaderMenu/Ignore leave the physical hold disabled (old semantics). This is
   independent of the Home-key classifier and stays behind `FREEINK_CAP_MENU_BUTTON`.
9. **A mapped action neutralizes held-time.** While the classifier has an action for this frame,
   `getHeldTime()` reports zero so a mapped Home event cannot also satisfy a generic long-press.

## 7. Dispatch placement

| Action | Site | Notes |
| --- | --- | --- |
| `Home` | `ActivityManager::loop()` | Existing non-home-screen `wasHomeGesture()` path |
| `NextPage` | Reader page-turn helper | Added to the `next` arm only |
| `Refresh` | `main.cpp` global loop | Combined with the existing short-power refresh branch |
| `ToggleFrontlight` | `main.cpp` global loop | Shared with power double-click |
| `Sleep`, `Screenshot`, `GoBack` | `main.cpp` global loop | Fork actions; `GoBack` uses current-activity back then stack pop |
| `Confirm` | Any activity's logical Confirm path | MappedInput aliasing |
| `Footnotes`, `Sync`, `Bookmark`, `Dictionary`, `ReaderMenu` | `EpubReaderActivity::loop()` | Upstream reader switch, adapted to the fork's block list |
| `Ignore` | nowhere | Classifier-only sentinel |

The reader switch runs after overlay/end-of-book ownership checks and before ordinary link/tap
handling, as upstream does. Non-page-turn reader actions clear `automaticPageTurnActive`, matching
upstream. TTF builds additionally treat `ttf_` as a valid book runtime alongside `section`.

The reader switch consumes `ReaderMenu` directly. The former
`openShortcutMenuOnCurrent()` / `Activity::openShortcutMenu()` helper has no remaining caller and is
deleted.

## 8. Settings UI and persistence plumbing

Adopt upstream's `SettingInfo::StaticEnum`, backed by `std::span<const StrId>` and
`SettingInfo::enumLabels()`. The generic JSON path clamps to that span; web GET/POST use the same
source. `home_button::isSetting()` keeps the raw static rows out of the ordinary category lists so
the dedicated Home Button activity is the only on-device editor.

Fresh-install defaults adopt upstream:

| Gesture | Default |
| --- | --- |
| Tap | `Home` |
| Double tap | `ToggleFrontlight` |
| Long press | `ReaderMenu` |

Existing files retain their meaning through §5. For menu-button-only boards, the Home Button
settings entry is shown on `FREEINK_CAP_HOME_KEY || FREEINK_CAP_MENU_BUTTON`; without that
extension, deleting the old Confirm-hold row would remove the only UI that drives
`homeButtonLongPressAction` on X4/X3/X4 Classic/Sticky.

## 9. Internationalization

Reuse existing IDs where semantics match: `STR_IGNORE`, `STR_PAGE_TURN`, `STR_FORCE_REFRESH`,
`STR_FOOTNOTES`, `STR_CONFIRM`, `STR_KOSYNC`, `STR_BOOKMARK_OPTION`, `STR_DICTIONARY`,
`STR_READER_MENU`, `STR_SLEEP`, `STR_SCREENSHOT_BUTTON`, `STR_GO_BACK`. Add only the upstream keys
the tree lacks (`STR_HOME_SHORTCUT`, `STR_HOME_BUTTON`, `STR_CONFIGURE`, dedicated double-tap and
long-press gesture labels, `STR_TOGGLE_FRONTLIGHT`, `STR_DBL_CLICK_PWR_LIGHT`). User-facing text
uses `tr()` / `I18N.get()`. Run `scripts/gen_i18n.py` through the PlatformIO pre-build step; commit
only `lib/I18n/translations/english.yaml`, not generated headers.

## 10. Tests

1. Port upstream `test/home_button/HomeButtonInputTest.cpp`, including window expiry, double tap,
   hold, swipe reset, second contact, millisecond wrap, and disabled double-tap latency.
2. Pin persisted enum values: upstream `0..10` and fork `11..13` / `Count=14` must not move.
3. Pin both migration maps from §5.1 and §5.3 as pure constants/helpers, including old-key fallback
   precedence and no remapping of already-migrated values `7..13`.
4. Keep/replace gesture tests so stall recovery and swipe reset are covered after
   `HomeTapTracker` is deleted.
5. Gate every push on: whole-tree `clang-format-fix`, `bin/cppcheck-check`, host `ctest`, and
   `pio run -e x4pro` plus `pio run -e default`.

The legacy maps and old-key precedence live in `src/util/HomeButtonMigration.h` as a pure seam so
host tests can pin them without instantiating the settings singleton or ArduinoJson-backed loader.

## 11. Ordered implementation

1. **Enums + settings + migration:** classifier/catalog headers, upstream fields/keys, migration,
   generic clamp, LP removal with consumers rewired in the same commit.
2. **Gesture engine swap:** MappedInput integration; delete `HomeTapTracker` and fork gesture latch;
   main-loop global dispatch with invariant guards.
3. **Reader/activity wiring:** reader switch, page turn, transition resets, transfer deferral,
   physical Confirm hold.
4. **Settings UI + i18n:** dedicated activity, `SettingAction::HomeButton`, static rows, web clamp,
   YAML keys.
5. **Tests:** upstream port plus fork value/migration pins and CMake wiring.

## 12. Decision log (append-only)

- **2026-09-15 — D1: adopt upstream names and wiring.** DRY wins: persisted enum values and gesture
  classification have one source. Upstream's file/class/key layout minimizes future merge conflicts.
  KISS rejects a compatibility enum.
- **2026-09-15 — D2: append fork values after upstream's last value.** SOLID wins over KISS: old
  saved bytes must not be silently reinterpreted. Reordering/reusing upstream values would corrupt
  user data.
- **2026-09-15 — D3: migrate in `fromJson` and request one resave.** DRY plus SOLID: `fromJson` is
  already the single legacy-format funnel and cannot corrupt by saving under `storeMutex`. The old
  double-click JSON key is the legacy marker because the new values 0..6 are valid too; value range
  alone would corrupt already-unified saves.
- **2026-09-15 — D3 gate deviation from upstream.** Upstream gates the LP-menu migration on
  `hasHomeKey()`, but fork Confirm hold lived on menu-button-only boards. Use
  `FREEINK_CAP_MENU_BUTTON` for that migration; extend the Home Button settings surface to home-or-
  menu-button boards. This is a deliberate feature-preserving extension, not a parallel catalog.
- **2026-09-15 — D4: keep fork guards inside upstream structure.** SOLID wins over upstream's
  shorter path: screen ownership, stale-window invalidation, stalled-loop recovery, and physical
  Confirm hold are correctness invariants. Additive gesture provenance is the minimum mechanism.
- **2026-09-15 — D4: classifier gesture provenance.** DRY plus SOLID: `HomeButtonInput` records the
  transition (tap, double tap, hold) that produced an action so main.cpp can enforce the fork's
  home-screen tap guard without reclassifying GPIO state. Upstream return values are unchanged.
- **2026-09-15 — D4: global/reader dispatch split.** DRY keeps one action source; placement preserves
  ownership. ToggleFrontlight, Refresh, Sleep, Screenshot, and GoBack run globally; NextPage and the
  book actions run in the reader; Confirm aliases the logical button; Home remains ActivityManager's
  navigation path. The obsolete shortcut-menu forwarding helper is deleted once callerless.
- **2026-09-15 — D4: delete duplicate gesture machinery.** KISS after DRY: once
  `HomeButtonInput::update()` has the same behavior, `HomeTapTracker`,
  `handleX4ProHomeDoubleClick()`, `deferredHomeGesture`, and `wasHomeKeyHold()` have no job.
- **2026-09-15 — D5: one dedicated settings screen.** DRY wins over repeating three enum rows;
  `home_button::ACTION_LABELS` is the only action-label source.
- **2026-09-15 — D6: upstream defaults for fresh installs.** KISS and upstream-merge compatibility
  win; the migration table preserves meaning for files that already exist.
- **2026-09-15 — D7: reuse IDs before adding keys.** DRY wins in translation space; generated
  headers stay out of git.
- **2026-09-15 — D8: test the migration policy, not the singleton.** KISS keeps the maps and
  old-key precedence in a pure header; full JSON-loader tests would drag firmware storage and I18n
  dependencies into host tests without covering additional persisted behavior.
- **2026-09-15 — historical docs.** Older design documents naming `HOME_ACT_*`, `HomeTapTracker`,
  or `longPressMenuFunction` are historical records. This document supersedes them; their stale
  terminology is not rewritten, but removed code must not leave live docs behind.
