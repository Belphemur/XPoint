# Touch Long-Press → Dictionary

## Summary

On boards with a capacitive touch panel, holding a finger on a word while
reading an EPUB looks that word up in the dictionary directly — no menu, no
walking a cursor across the page with Left/Right.

This is the **default** behaviour on touch boards, with a
`Settings → Controls → Long-press on Text` setting to change it (Dictionary /
Ignore / Footnote).

## Motivation

The dictionary already exists and works well
([`DictionaryWordSelectActivity`](../../src/activities/reader/DictionaryWordSelectActivity.cpp)),
but reaching a specific word is slow:

1. Open the reader menu (Confirm / center tap) → **Look Up**, or set
   `Long-press Menu` to Dictionary and hold Confirm.
2. Word-select opens with the highlight on *the middle row's word nearest
   mid-screen* (`onEnter()`, `DictionaryWordSelectActivity.cpp:52-57`) — because
   neither trigger carries a coordinate.
3. Walk to the word you actually wanted with Left/Right/Up/Down.

A touch long-press is the one gesture that *does* carry an `(x, y)`. Pointing at
the word is the natural interaction, and the machinery to resolve a coordinate
to a word already exists (`wordAt(x, y)`, `DictionaryWordSelectActivity.cpp:114`).

### Why this matters most on the X4 Pro

The X4 Pro has **no Confirm button**. Its `InputPins`
([`BoardConfig.h:1432`](../../freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h))
is:

```
// {back, confirm, left, right, up, down, power, powerActiveHigh}
{PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, PIN_UNASSIGNED, 0, 7, 3, false}
```

Only Left=GPIO0, Right=GPIO7, Power=GPIO3 exist; the board comment states
*"back/confirm come from the GT911 (touch + the capacitive Home key)"*, and its
`TouchConfig.synthesizeConfirm` is `false`, so a tap does not synthesize Confirm
either. Touch long-press is therefore the only ergonomic dictionary trigger on
that device.

## Current behaviour (verified)

### What `longPressMenuFunction` actually is

`SETTINGS.longPressMenuFunction` (`CrossPointSettings.h:173-180`) is **not** a touch
setting. Enum: `LP_MENU_KOSYNC=0, LP_MENU_DISABLED=1, LP_MENU_BOOKMARK=2,
LP_MENU_DICTIONARY=3, LP_MENU_READER_MENU=4`, default `LP_MENU_DISABLED`.
Surfaced as `STR_LONG_PRESS_MENU` via `buildLongPressMenuValues()`
(`SettingsList.h:186`, which already hides `Reader Menu` on non-`hasHomeKey()`
boards). It is dispatched from exactly two places in
`EpubReaderActivity::loop()`:

| Trigger | Site | Requires |
| --- | --- | --- |
| Physical **Confirm** hold | `EpubReaderActivity.cpp:577-602` | a `confirm` GPIO (or `synthesizeConfirm`) |
| Capacitive **Home key** hold | `EpubReaderActivity.cpp:607-636` | `BoardConfig::hasHomeKey()` |

The hold threshold is action-dependent (`confirmLongPressThreshold()`,
`:1039-1051`): `BOOKMARK_HOLD_MS` for Bookmark/Dictionary, `GO_HOME_MS` for
KOSync (and `0` — disabled — for Reader Menu/Disabled).

Neither trigger carries a coordinate. That is the whole reason word-select opens
mid-page.

### Touch long-press is available and currently unused in the reader

`MappedInputManager::wasScreenLongPress(int& x, int& y)`
(`MappedInputManager.cpp:162-172`) already exists and is fed by
`HalGPIO::wasTouchLongPress` → `InputManager`:

- Fires **once per contact**, while the finger is still down, at
  `TOUCH_LONG_PRESS_MS = 500` (`InputManager.h:429`).
- Only when the contact is **stationary**: `!touchMovedBeyondTapSlop &&
  !touchMultiContactSequence && !touchSuppressed`
  (`InputManager.cpp:1162-1165`). A swipe can therefore never become a
  long-press.
- Consuming it calls `gpio.suppressTouchContact()`, which latches
  `touchSuppressed` so the rest of the contact is dead — the finger lift cannot
  also register as a tap (`MappedInputManager.cpp:166-168`; the tap/swipe
  classifiers all bail on `touchSuppressed`, `InputManager.cpp:580-720`).

**No reader path reads it today.** The only consumer in the tree is the list-UI
snapshot helper (`UiAppHelpers.h:170`, opt-in via `withLongPress`; callers
`UiAppHost.cpp:25`, `OptionPopup.h:72`, `ClockOffsetActivity.cpp:199`). So wiring
the reader to it introduces no *gesture* conflict — swipes and taps are
structurally excluded, per the classifier gates above.

**It does change one behaviour, deliberately.** `wasTouchTap` has **no duration
cap** (`InputManager.cpp:592-600`): today a stationary 5-second hold followed by a
lift still counts as a tap, so in the tap-page-turn modes
(`TOUCH_READER_ON` / `TOUCH_READER_INVERTED_TAP`) a long hold inside a tap zone
turns the page on lift. Once this feature consumes the long-press, that hold
performs the lookup and **no longer turns the page**. In the default
`TOUCH_READER_SWIPE` mode taps don't turn pages at all, so nothing changes there.
`TOUCH_LP_IGNORE` preserves today's behaviour exactly (the event is never read, so
the contact is never suppressed).

### The touch flag

`BoardConfig::hasTouch()` (`BoardConfig.h:1664`,
`ACTIVE.touch.controller != TouchController::None`) → `HalGPIO::hasTouch()` →
`MappedInputManager::hasTouch()`. `SettingsList.h:493-496` already uses it to
erase `STR_TOUCH_READER_CONTROLS` on button-only boards — the same shape this
design reuses.

## Design

### New setting

Add to `CrossPointSettings.h`, next to the other touch settings:

```cpp
// Action for a long press on the reading surface (touch boards only).
// Persisted as uint8_t BY MENU-POSITION INDEX; APPEND-ONLY.
enum TOUCH_LONG_PRESS_ACTION {
  TOUCH_LP_DICTIONARY = 0,
  TOUCH_LP_IGNORE = 1,
  TOUCH_LP_FOOTNOTE = 2,
  TOUCH_LONG_PRESS_ACTION_COUNT
};

uint8_t touchLongPressAction = TOUCH_LP_DICTIONARY;  // default per feature intent
```

**A new enum, not a reuse of `longPressMenuFunction`** — deliberately:

- That catalog contains `KOSYNC` and `READER_MENU`, which are meaningless or
  redundant on a coordinate-carrying gesture (the reader menu is already a
  center tap / bottom-edge swipe).
- Its stored `uint8_t` is shared with the Confirm-hold and Home-hold paths;
  appending touch-only entries would offer them on the button triggers too.
- A user may legitimately want Bookmark on Confirm-hold *and* Dictionary on
  touch long-press at the same time. One shared field cannot express that.

`SettingsList.h`: emit an `Enum` entry keyed `"touchLongPressAction"` under
`STR_CAT_CONTROLS` with values `{STR_DICTIONARY, STR_IGNORE, STR_FOOTNOTES}`,
then erase it when `!BoardConfig::hasTouch()` in the same block that erases
`STR_TOUCH_READER_CONTROLS`. **All three value strings already exist** in
`lib/I18n/translations/english.yaml` (`STR_DICTIONARY:195`, `STR_IGNORE:182`,
`STR_FOOTNOTES:394`) — so only the row label below is new.

A new `STR_TOUCH_LONG_PRESS` label key *is* required for the row name. Adding it
means editing **only** `lib/I18n/translations/english.yaml`: the `StrId` enum now
lives in `lib/I18n/I18nKeys.h`, which is **generated** by
`scripts/gen_i18n.py` (`pre:` hook in `platformio.ini`) and is **gitignored**
(`.gitignore:8-10`) — there is no hand-maintained enum header to edit any more,
and the generator auto-fills the other languages from English.

### Trigger, and its gating

In `EpubReaderActivity::loop()` (exact placement below):

```cpp
// Gate 1 — feature enabled on this board (explicit OFF check, not bare truthiness)
if (SETTINGS.touchReaderControls != CrossPointSettings::TOUCH_READER_OFF &&
    mappedInput.hasTouch() &&
    // Gate 2 — user did not opt out of the gesture
    SETTINGS.touchLongPressAction != CrossPointSettings::TOUCH_LP_IGNORE &&
    // Gate 3 — not mid-popup: don't re-enter from the transient "no dictionary" toast
    !showDictionaryMessage &&
    // Gate 4 — not mid auto-page-turn: coords would be stale after the flip
    !automaticPageTurnActive) {
  int lx = 0, ly = 0;
  if (mappedInput.wasScreenLongPress(lx, ly)) {   // self-suppresses the contact
    // pass the action explicitly — the activity must NOT read the mutable global
    const auto mode = static_cast<TouchLongPressMode>(SETTINGS.touchLongPressAction);
    openDictionaryWordSelect(lx, ly, mode);
    return;
  }
}
```

- Gated on `SETTINGS.touchReaderControls` being non-`TOUCH_READER_OFF`, matching
  the reader-menu rule (`isTouchMenuGesture`, `ReaderUtils.h:148`): with touch
  reader controls Off the reading surface ignores touch **entirely**, so a stray
  brush cannot trigger anything.
- `TOUCH_LP_IGNORE` short-circuits **before** `wasScreenLongPress()` is called,
  so the event is left unconsumed and no `suppressTouchContact()` happens — a
  hold then still behaves exactly as today (a slow tap → page turn on lift).
**Exact placement in `loop()` matters.** Insert the check **after** the toolbar
overlay early-return (`EpubReaderActivity.cpp:562-571`, which owns all input while
an overlay is up) and **after** the `automaticPageTurnActive` block (`:526-550`),
but before the Confirm-hold dispatch (`:577-602`):

- **Auto-page-turn race:** if auto-turn flips the page while the finger is held,
  the long-press coordinates were captured against the *old* page while
  `openDictionaryWordSelect()` loads the *current* one → wrong-word lookup. Guard
  by ignoring the long-press while `automaticPageTurnActive` is set (the existing
  block already cancels auto-turn on a touch menu gesture, `:526-531`).
- **Re-entry guard:** mirror the home-hold path's `!showDictionaryMessage` check
  (`:622`) so a long-press during the transient "no dictionary" message cannot
  re-enter `openDictionaryWordSelect()`.
- The long-press and the Confirm-hold cannot collide: no board has both a touch
  panel long-press and a physical Confirm hold arriving in the same frame from the
  same contact.
- **Multi-touch ordering:** long-press classification requires
  `!touchMultiContactSequence`, and `suppressTouchContact()` also cancels
  multi-touch (`InputManager.cpp:747-756`). This feature and the two-finger
  swipe/rotate gestures now share the suppress latch — whoever reads the contact
  first wins it.

**Implementation footgun — `wasTouchLongPress()` is NOT read-clearing.** It gates
on `touchLongPressEvent` and `touchMultiContactSequence` but **not** on
`touchSuppressed` (`InputManager.cpp:734-745`), and `suppressTouchContact()` does
not clear the event. Only *re-setting* is latched (`touchLongPressFired`, `:1164`);
the event flag itself is cleared at the top of the next `update()`
(`InputManager.cpp:456`). So it returns `true` on **every** poll for the rest of
that frame. The consumer here is safe because it navigates away on the first read,
but the reader must call it **once**, in one place, and dispatch from that single
result — never re-poll to re-check inside the dispatch.

`wasScreenLongPress()` reports the **touch-down** position, not the current
finger position (`InputManager.cpp:737-738`, same rationale as `wasTouchTap`), so
a finger that lands between words and drifts a few pixels still resolves at the
point where it first touched down.

### Dictionary action (default)

Extend the existing entry point with an optional coordinate **and an
explicit mode**. The activity must never read the mutable global
`SETTINGS.touchLongPressAction`: the existing normal-dictionary callers
(confirm-hold `:595`, home-hold `:622`, menu Look Up `:948`) all pass no mode and
must stay in Dictionary mode even when the touch setting is Footnote — otherwise
the global would silently flip every dictionary open into footnote mode.

```cpp
// Explicit contract: the activity never reads the mutable global.
enum class TouchLongPressMode { Dictionary, Footnote };

void EpubReaderActivity::openDictionaryWordSelect(
    int touchX = -1, int touchY = -1,
    TouchLongPressMode mode = TouchLongPressMode::Dictionary);
```

It already loads the current `Page` and computes the oriented margins
(`EpubReaderActivity.cpp:416-436`); it forwards the coordinate **and the mode**
to the activity, whose constructor gains `initialX/initialY` (default `-1`) and
`mode` (default `Dictionary`). In `onEnter()`, **after** `extractWords()`:

```cpp
if (initialX >= 0) {
  const int hit = wordAt(initialX, initialY);   // existing, with finger slop
  if (hit >= 0) {
    selected = hit;
    if (mode == TouchLongPressMode::Footnote && isFootnoteMarker(words[hit].text)) {
      // resolve to href, return it via ActivityResult; reader calls navigateToHref
    } else {
      performLookup();                           // immediate, per design decision
    }
    return;
  }
}
// no word under the finger (or a Footnote-mode miss on a non-marker) → existing
// mid-page default highlight
```

`wordAt()` already applies `SLOP = 4` px around each box for finger error
(`DictionaryWordSelectActivity.cpp:114`), and the boxes are in the same logical
coordinate space `wasScreenLongPress()` returns (both go through
`renderer.tapToLogical`, and word boxes are built from the same oriented margins
the reader renders with).

**Flow / back-stack** (decided): the definition opens immediately;
`Back` from the definition returns to **word-select** with the word still
highlighted, so a mis-hit is corrected with Left/Right instead of re-doing the
gesture; `Back` again returns to the reader. This is exactly the existing
`performLookup()` behaviour — word-select stays on the stack beneath the
definition — so it needs no new navigation code.

**Miss handling**: a long-press on whitespace, a margin, or an image falls back
to the normal mid-page highlight rather than closing, so the gesture is never a
dead end. No dictionary configured is already handled at the top of
`openDictionaryWordSelect()` (`dictionaryName[0] == '\0'` → transient
"no dictionary" message).

### Footnote action

Follow the footnote/link under the finger, falling back to the dictionary when
the touched word is not a marker.

`FootnoteEntry` (`lib/Epub/Epub/FootnoteEntry.h:11`) stores **only**
`number[32]` + `href[256]` — **no coordinates**. Footnotes are attached to a
page by cumulative word index during layout
(`lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:2105-2112`) and the geometry is
then discarded. This design therefore matches **by marker text**.

**Normalize the word side only; compare the entry verbatim.** The parser
*already* normalizes `FootnoteEntry.number` when it collects it
(`ChapterHtmlSlimParser.cpp:1536-1562`): it strips leading whitespace/`[` and
trailing whitespace/`]`, so `"  [ 12 ]  "` is stored as `"12"`. The page's word
text, by contrast, is **verbatim** — a `[1]` on the page stays `[1]`, because the
char handler falls through into normal word accumulation after collecting the
marker (`:1564+`). So the comparison must mirror the parser's transform on the
**touched word**, and leave the entry untouched:

```
normalizeMarker(word)  // strip leading ws/'[', trailing ws/']' — mirrors :1546-1554
  == entry.number      // already normalized by the parser; compare verbatim
```

**Do NOT strip `()`.** The parser does not strip parentheses, so a `(1)`-style
marker is stored as `(1)`; trimming the entry side would turn it into `1` and
break the match. Stripping only `[`/`]` on the word side keeps `[1]`, `1` and
`(1)` all working.

**Where the hit-test lives (decided):** inside `DictionaryWordSelectActivity`,
not the reader. The reader has no word geometry — resolving a coordinate to word
*text* requires `extractWords()`, a private method that builds the word vector
(`DictionaryWordSelectActivity.cpp:60-109`). Duplicating it reader-side would
copy ~50 lines **and** add a new per-gesture heap allocation in the reader. So
the Footnote action opens the same activity with the same `initialX/initialY` and
`mode = TouchLongPressMode::Footnote`, and the activity branches on entry.

**The fallback rule, stated where it is defined** (so two implementations cannot
diverge): after the word hit,

- if the hit word normalizes to a `FootnoteEntry.number` → return the href via
  `ActivityResult`; the reader calls `navigateToHref(href, true)`.
- else if the touched token is **purely numeric** (e.g. `1`, `12`) and matched
  no entry → **do NOT fall through to the dictionary** (a numeric lookup is a
  guaranteed miss and a wasted popup). Fall back to the mid-page highlight (or a
  silent no-op) exactly as a whitespace miss does.
- else (a real word, not a marker) → `performLookup()`, the normal dictionary
  path.

This numeric-exception is part of the contract, not just a limitation: a
footnote-bearing page commonly has many bare numbers in body text, and sending
every one of them to the dictionary would make the Footnote action feel broken.

Returning the href through the result (rather than navigating from inside the
child) matters: it lets the reader `finish()` word-select **first**, so the
footnote destination does not sit on top of a stale word-select activity. Reuse
the exact call pattern of the existing power-button footnote path
(`EpubReaderActivity.cpp:663-684` → `navigateToHref(href, /*savePosition=*/true)`,
signature at `EpubReaderActivity.h:177`), so the existing Back-restore path
(`:655-659`, `pwrBtnFootnoteBack`) engages unchanged.

**The footnote list is read from the activity's *owned* `Page`
(`page->footnotes`) — decided, no implementation-time choice left.** This is the
only correct source. `openDictionaryWordSelect()` loads a **fresh** `Page` via
`section->loadPage(section->currentPage)` and `std::move`s it into the activity
(`EpubReaderActivity.cpp:421-433`); the activity stores it as
`std::unique_ptr<Page> page` (`DictionaryWordSelectActivity.h:51`). That fresh
page still carries its own `footnotes` vector (footnotes are attached during
parsing, `ChapterHtmlSlimParser.cpp:2105-2112`, and are only stripped from the
*reader's* separate page object when the reader does
`currentPageFootnotes = std::move(p->footnotes)` at `:1516`). The activity's page
and the reader's `currentPageFootnotes` are therefore two distinct copies; the
reader's is empty inside the activity and must never be consulted. So the
activity branches on `page->footnotes` after `extractWords()`, with no
shared-state ambiguity. Assert in the host test that the owned page's
`footnotes` vector is non-empty for a footnote-bearing test page.

**Explicitly rejected: real link boxes.** Carrying a marker's word index into
`PageLine`/`Page` would give exact hit-testing (and would be reusable for
highlights later), but it changes `Page::serialize`/`deserialize`
(`lib/Epub/Epub/Page.cpp`), which forces a **page-cache version bump and a
re-paginate of every book on the device**. That blast radius is not justified by
this feature; it can be revisited if/when highlights are built.

**Known limitations** (documented, not fixed) — all degrade to a dictionary
lookup or a no-op, never to a jump to the wrong chapter, because the href always
comes from a real `FootnoteEntry` on *this* page:

- **Multi-word markers cannot match by construction.** The parser preserves
  interior text, so a `"turn to 256"` noteref (its own documented example,
  `:1543`) spans several word boxes and no single `wordAt` hit can equal it.
- **Repeated numbers on one page** are ambiguous; first matching entry wins.
- **Markers glued to the preceding word** by the layout engine won't match.
- **Pages with >16 footnotes** silently drop the 17th+
  (`MAX_FOOTNOTES_PER_PAGE`, `Page.h:79`; `addFootnote` returns early at `:88`),
  so a touched marker beyond the cap matches nothing.
- **Numeric fallback is a dead end:** falling back to a dictionary lookup of
  `1` or `12` will almost always show "not found". Prefer suppressing the
  dictionary fallback when the touched token is purely numeric and the action is
  Footnote — cheaper than a guaranteed-miss dictionary query.
## Note: `Long-press Menu` reachability

`longPressMenuFunction` is a Confirm-hold setting only. On boards without a
Confirm pin the row is hidden in Settings and not dispatched.
## Alternatives considered

| Option | Verdict |
| --- | --- |
| Extend `longPressMenuFunction` with touch entries | Rejected — shared stored index across button/touch paths; would offer KOSync/Reader-Menu on a coordinate gesture |
| Reuse the Home-button action catalog (`HOME_BUTTON_ACTION`) | Rejected — those actions (Frontlight, Sleep, Screenshot, Go Home) are coordinate-blind and already reachable from the Home key; it would waste the one gesture that carries an `(x, y)` |
| Bookmark on touch long-press | Rejected for now — coordinate-blind, duplicates the existing Confirm-hold Bookmark option |
| Reader menu on touch long-press | Rejected — already a center tap / bottom-edge up-swipe |
| Highlight / annotate the word | Out of scope — no highlight persistence infrastructure exists |
| Translate / Wikipedia lookup | Rejected — requires network; contrary to the lean, offline-first device bias |
| Open word-select with the word pre-highlighted, require Confirm to look up | Rejected — an extra confirmation for a gesture whose entire point is directness; a mis-hit is already correctable via Back |

## Resource impact

Deliberately near-zero, per the 380 KB RAM ceiling:

- **RAM**: +1 `uint8_t` in `CrossPointSettings`. The dictionary path allocates
  nothing new — it reuses the existing `Page` load and the word vector that
  `DictionaryWordSelectActivity` already builds, entered one frame earlier than
  before. Two `int` parameters are passed by value on the stack.
- **Flash**: one settings row, one dispatch `switch`, the footnote
  number-comparison helper. No new activity, no new asset.
- **I/O**: no new SD reads on the gesture path. The settings write is the
  existing debounced `PersistableStore` save, only when the user changes the
  value. The footnote action reads `page->footnotes`, already in RAM.
- **CPU**: `wordAt()` is a linear scan over the page's word boxes (the vector is
  `reserve(128)`d, `DictionaryWordSelectActivity.cpp:62` — a hint, not a cap; dense
  pages grow it), once per gesture
  — the same cost already paid for every touch-down in word-select.
- **No new heap allocation**, so nothing to justify under the resource protocol.

## Verification

Host unit tests (`test/` already has 17 suites + CMakeLists, run by
`ci.yml:161-188`), highest value first:

1. **`wordAt` × orientation — the most likely silent failure.** Build a synthetic
   page of word boxes and assert that a point pushed through
   `GfxRenderer::tapToLogical` (`GfxRenderer.cpp:1850-1877`) resolves to the
   expected word in **all four** orientations. This is fully host-testable and is
   the one piece of new logic that can mis-hit without any visible error.
2. **Footnote marker normalisation/match**: word-side strip of `[`/`]`/whitespace
   compared against a verbatim entry — `[1]`→`1` matches, `(1)` matches `(1)`,
   `¹` matches, multi-word `"turn to 256"` correctly does **not** match, and a
   purely-numeric miss does not fall through to a doomed dictionary query.
- `pio run -e x4pro` (the board this targets) and `pio run -e default` (verify
  the non-touch build still compiles with the setting erased).
- `./bin/clang-format-fix` + `pio check` before pushing.
- On device (X4 Pro): long-press a word → definition; Back → word-select with
  that word highlighted; Back → reader at the same page. Long-press whitespace
  → mid-page highlight. Long-press a footnote marker with the action set to
  Footnote → jumps to the note. Set the action to Ignore → hold behaves as
  before (no lookup, no accidental page turn on lift). Set Touch Reader Controls
  to Off → gesture inert. Confirm a swipe still turns the page and never fires a
  lookup. In `TOUCH_READER_ON`/`INVERTED_TAP` modes, confirm the deliberate change:
  a ≥500 ms hold in a tap zone looks the word up instead of turning the page, while
  a quick tap still turns it. Re-test the hit-test in landscape as well as portrait.

## Files touched

This PR ships the **design document and its implementation** (the design was the
review gate; the code followed once approved). The implementation landed in two
feature commits on top of the two design-doc commits.

**Design (this PR)**

| File | Change |
| --- | --- |
| `docs/design/touch-long-press-dictionary.md` | this document |

**Implementation — `feat(reader): touch long-press looks up the word in the dictionary`**

| File | Change |
| --- | --- |
| `src/CrossPointSettings.h` | `TOUCH_LONG_PRESS_ACTION` enum + `touchLongPressAction` field |
| `src/SettingsList.h` | new Enum row `STR_TOUCH_LONG_PRESS`, erased when `!hasTouch()` |
| `lib/I18n/translations/english.yaml` | `STR_TOUCH_LONG_PRESS` row label (values reuse existing keys; `I18nKeys.h` is generated + gitignored) |
| `src/activities/reader/TouchLongPressMode.h` | shared `TouchLongPressMode` enum (Dictionary / Footnote) |
| `src/activities/reader/EpubReaderActivity.{h,cpp}` | long-press trigger + dispatch with explicit `TouchLongPressMode`; `openDictionaryWordSelect(x, y, mode)`; footnote href → `navigateToHref` |
| `src/activities/reader/DictionaryWordSelectActivity.{h,cpp}` | `initialX/initialY` + `mode` → immediate lookup in `onEnter()`; footnote-marker branch returning the href via `ActivityResult` |

**Implementation — `fix(reader): let Long-press Menu reach the reader when no Confirm button exists`**

| File | Change |
| --- | --- |
| `src/main.cpp` | Home-hold fall-through when the board has no Confirm trigger, so a configured `longPressMenuFunction` is not permanently shadowed (see "Related finding" section) |
| `src/activities/reader/EpubReaderActivity.cpp` | `LP_MENU_KOSYNC` Home-key case falls through to the Home action when credentials are absent (it must not consume the hold it cannot serve) |
| `src/activities/ActivityManager.{h,cpp}` | `isCurrentActivityReader()` predicate so the X4 Pro fallback scopes to the foreground reader, not a reader buried in the activity stack |

**`freeink-sdk`**: **unchanged** — no submodule bump; board fields are read via
`BoardConfig::ACTIVE.*` (the app already does this at `UIThemeTokens.h:29`,
`main.cpp:500`).

**Not in this PR (intentionally):** `docs/dictionary.md` / `USER_GUIDE.md` user-facing
prose, and `test/` host suites for the footnote normalisation + `wordAt` × 4
orientations — candidates for a follow-up once on-device feel is confirmed.

## Review pass applied

An external-model review (Kimi-K3, repo mounted, asked to verify every cited
claim against the actual tree) was run on the first draft. Findings folded in:

- **The X4 Pro section was materially wrong and has been rewritten.** The draft
  proposed hiding `STR_LONG_PRESS_MENU` when the board has no Confirm trigger and
  no Home key — but the X4 Pro sets `hasHomeKey = true` (`BoardConfig.h:1470`), so
  the gate would never have fired on the board the section was about, while
  wrongly hiding the row on PaperMono/M5PaperS3. Reframed as a defaults/arbiter
  fix, and the affected-board table corrected.
- **The footnote matcher was normalising the wrong side.** The parser already
  strips `[`/`]`/whitespace from `FootnoteEntry.number`
  (`ChapterHtmlSlimParser.cpp:1536-1562`); the draft's "trim `[]()`" would have
  broken `(1)`-style markers. Now: normalise the **word** side only, compare the
  entry verbatim, never strip `()`.
- **The footnote hit-test now has an explicit home** (in-activity, href returned
  via `ActivityResult`), resolving a contradiction with the "no new heap
  allocation" claim.
- **"Cannot regress page turns" was false** in tap-page-turn modes and is now
  stated as a deliberate, mode-specific behaviour change.
- Added the auto-page-turn race, the overlay/`showDictionaryMessage` guards, the
  multi-touch suppress-latch ordering note, the `>16 footnotes` and multi-word
  marker limitations, and the correct `wasTouchLongPress` read semantics
  (not read-clearing; reports the touch-down point).
- ~15 stale line citations corrected against the tree at `fa10d96b`.

Claims the review independently **confirmed**: the coordinate spaces match (word
boxes and `tapToLogical` share the same logical frame and margin origin), marker
text does survive into the page word list, the gesture primitive fires once per
contact and structurally excludes swipes/taps, enum persistence is by menu
position (so append-only is required), `navigateToHref` and the i18n/board field
paths exist as described, and the resource claims are plausible for the RAM
ceiling.
