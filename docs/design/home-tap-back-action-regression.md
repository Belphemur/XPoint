# Design: Hardware-Gate Long-Press Features with Build-Time Macros

**Status:** Implemented (worktree under review)
**Date:** 2026-09-01
**Worktree:** `crosspoint-worktree-fix-home-tap-back` (branch `fix/home-tap-back-action`, off `develop @ f4308b84`)

## 1. Symptom

On X4 Pro (capacitive Home key + touch + no Confirm pin), a single Home press
in the EPUB reader opened the **Dictionary Word Select** screen, ignoring the
user's configured Home Button Tap setting (Go back one level).

## 2. Principle (user-stated)

> No home button, no home-button feature. No confirm button, no confirm-button feature. Gate properly with a compile macro.
>
> Also can we have the python simple parse the header file instead of having own dictionary? Or even have it loaded directly? We should avoid duplication DRY.

Two distinct layers:

| Layer | What it does | Mechanism |
| --- | --- | --- |
| Compile-time | Removes the code from the binary entirely. Settings rows, dispatchers, helper functions all disappear if the feature is absent. | `FREEINK_CAP_HOME_KEY` / `FREEINK_CAP_MENU_BUTTON` macros in the generated `build/board_features.h`. |
| Runtime | Defense in depth. If somehow the macro is mis-set, the existing `BoardConfig::hasHomeKey()` / `confirm != PIN_UNASSIGNED` checks still short-circuit. | Already present in `SettingsList.h` and `MappedInputManager.cpp`. |

The compile-time gate is the **source of truth**. The runtime check is a
belt-and-braces fallback.

## 3. Macro contract

### Naming

`FREEINK_CAP_HOME_KEY` and `FREEINK_CAP_MENU_BUTTON`, both `0` or `1`,
following the existing `FREEINK_CAP_*` namespace in `BoardConfig.h`
(`FREEINK_CAP_TOUCH`, `FREEINK_CAP_FRONTLIGHT`, `FREEINK_CAP_WARMLIGHT`, ...).
Consumers use `#if FREEINK_CAP_HOME_KEY` / `#if FREEINK_CAP_MENU_BUTTON`.

### DRY chain (single source of truth)

```text
BoardConfig.h ────► board_features_dump.c ──JSON──► gen_board_features.py ──► build/board_features.h
  (SDK truth)        (host C++ dump)                 (thin emitter)           (FREEINK_CAP_*)
```

1. `freeink-sdk/.../BoardConfig.h` defines every board's `InputPins.confirm`
   and `TouchConfig.hasHomeKey`. No capability dictionary lives anywhere else.
2. `scripts/board_features_dump.c` includes the real `BoardConfig.h` on the
   host (minimal stubs for `<Arduino.h>`, `<driver/gpio.h>`, `<esp_rom_sys.h>`
   live in `scripts/hoststubs/`) and prints each device's flags as JSON,
   keyed by `FREEINK_DEVICE_*` name. Per-board detail goes to stderr for the
   audit log. Because `BoardConfig.h`'s build-composition check requires a
   coherent single-MCU device set while its board profiles are ungated
   `constexpr` data, the dump compiles with the C3 pair (X3+X4) and reads
   every profile constant directly.
3. `scripts/gen_board_features.py` compiles/runs the dump, reads the active
   env's `-DFREEINK_DEVICE_*` set from `platformio.ini`, ORs the flags across
   active devices, and writes `build/board_features.h` plus an audit trail
   `build/board_features.audit.txt` citing the exact `BoardConfig.h` lines
   per board. An env device flag with no dump entry fails the build loudly.
4. `src/BoardFeatures.h` is a thin wrapper that includes the generated
   header (relative include `../build/board_features.h`, resolved against
   `src/`); `build` is gitignored.

### Per-device feature table (truth-checked from the dump)

| Device (`FREEINK_DEVICE_*`) | Home key | Menu button | Profile(s) (`constexpr BoardProfile`) |
| --- | --- | --- | --- |
| `X4` | no | yes | `XTEINK_X4` |
| `X3` | no | yes | `XTEINK_X3`, `XTEINK_X3_UC8279` |
| `X4PRO` | yes | no | `XTEINK_X4_PRO` |
| `X4CLASSIC` | no | yes | `XTEINK_X4_CLASSIC` |
| `M5` | no | yes | `M5STACK_PAPER_COLOR` |
| `MURPHY` | no | yes | `MURPHY_M3` |
| `MURPHY_M4` | no | yes | `MURPHY_M4` |
| `DELINK` | no | yes | `DE_LINK` |
| `LILYGO` | yes | no | `LILYGO_T5S3` |
| `M5PAPER` | no | yes | `M5PAPER_V11` |
| `STICKY` | no | yes | `STICKY` |
| `PAPERMONO` | no | no | `PAPER_MONO` |
| `PAPERS3` | no | no | `M5PAPER_S3` |
| `EEGO_A4` | yes | no | `EEGO_A4` |
| `ONEPAGE` | no | no | `ONEPAGE` |

Derived from `BoardConfig.h:500` (`int8_t confirm;`), `:520`
(`bool synthesizeConfirm;`), `:544` (`bool hasHomeKey = false;`) and each
profile's initializer; the generator re-derives these citations into
`build/board_features.audit.txt` on every run.

### Why macros, not pure runtime checks

- **Flash savings.** Boards without a Home key drop the arbiter, the three
  home-button settings fields and rows entirely; boards without a Confirm
  button drop the reader's Confirm-hold consumer and the long-press-menu row.
- **Compile-time guard.** A new `FREEINK_DEVICE_*` that the dump does not
  know fails generation with a non-zero exit instead of silently shipping a
  half-configured binary.
- **No drift.** `BoardConfig.h` stays the only place a board's pins are
  described; `platformio.ini` stays the only place an env's device set is
  declared.

## 4. Code wrapped by each macro

### Macros

| Macro | Set by | Gates |
| --- | --- | --- |
| `FREEINK_CAP_HOME_KEY` | `gen_board_features.py` from the dump's `home_key` | `CrossPointSettings.h` home-button fields; `SettingsList.h` `STR_HOME_BUTTON_*` rows; `main.cpp` `handleX4ProHomeDoubleClick()` + `executeHomeButtonAction()` |
| `FREEINK_CAP_MENU_BUTTON` | `gen_board_features.py` from the dump's `menu_button` | `SettingsList.h` `STR_LONG_PRESS_MENU` row; `EpubReaderActivity.cpp` Confirm-hold consumer |

### `FREEINK_CAP_HOME_KEY` gates

| Site | What is wrapped |
| --- | --- |
| `src/CrossPointSettings.h` | The three `homeButton{Tap,DoubleClick,LongPress}Action` fields. The `HOME_BUTTON_ACTION` enum stays outside the gate (its values are stored as `uint8_t` in JSON; if the feature is ever re-enabled on a saved-settings board, the enum must still compile). |
| `src/SettingsList.h` | The three `STR_HOME_BUTTON_*` SettingInfo rows. |
| `src/main.cpp` | `homeTapTracker`/`X4PRO_HOME_DOUBLE_CLICK_MS`, `executeHomeButtonAction()`, and the entire `handleX4ProHomeDoubleClick()` — replaced with a `return false;` stub on boards without a Home key. The call site already handles `false`. |

### `FREEINK_CAP_MENU_BUTTON` gates

| Site | What is wrapped |
| --- | --- |
| `src/SettingsList.h` | The `STR_LONG_PRESS_MENU` SettingInfo row. |
| `src/activities/reader/EpubReaderActivity.cpp` | The Confirm-hold consumer (`wasLongPressed(Button::Confirm, ...)` + the `switch (SETTINGS.longPressMenuFunction)`), re-added inside the gate; `endOfBookMenuOpen` is scoped to the gate. |

### Not gated (always built)

- `src/util/HomeTapTracker.h` — pure header-only state machine, unit-tested
  on every host.
- `MAPPED_INPUT` getters in `MappedInputManager` — thin wrappers over
  `gpio.*` calls that already short-circuit on boards without the hardware.
- The `longPressMenuFunction` and `HOME_BUTTON_ACTION` enums in
  `CrossPointSettings.h` — JSON-stored values must round-trip even when the
  feature is disabled.
- The runtime `erase()` checks in `SettingsList.h` — defense-in-depth.

### Already-removed code that stays removed

The home-button **override** code — the reader's `wasHomeKeyHold()`
consumer and the no-Confirm-pin fall-through branch in
`src/main.cpp:handleX4ProHomeDoubleClick()` — was deleted by the previous
run and is **not** re-added under either macro. The user explicitly stated
"anything that was overriding the home feature we built should be fully
removed."

## 5. Files touched (final)

1. `scripts/board_features_dump.c` — new. Host C++ dump of the real board
   profiles → JSON (stdout) + per-board audit lines (stderr).
2. `scripts/hoststubs/{Arduino.h,driver/gpio.h,esp_rom_sys.h}` — new.
   Minimal parse-only stand-ins for the dump's ESP-only includes.
3. `scripts/gen_board_features.py` — new. Runs the dump, resolves the env's
   device set, emits the header + audit log; also a PlatformIO pre-script.
4. `src/BoardFeatures.h` — new. Wrapper over `build/board_features.h`.
5. `src/CrossPointSettings.h` — include the wrapper; gate the three
   home-button fields.
6. `src/SettingsList.h` — include the wrapper; gate the three
   `STR_HOME_BUTTON_*` rows and the `STR_LONG_PRESS_MENU` row.
7. `src/main.cpp` — include the wrapper; gate the home-key arbiter,
   `executeHomeButtonAction()` and the tracker constants.
8. `src/activities/reader/EpubReaderActivity.cpp` — restore the
   Confirm-hold consumer inside `#if FREEINK_CAP_MENU_BUTTON`.
9. `test/home_tap_tracker/HomeTapTrackerTest.cpp` — regression test from
   the previous run (unchanged).
10. `platformio.ini` — append `pre:scripts/gen_board_features.py` to
    `extra_scripts`.
11. `docs/design/*.md` — this document plus the two design notes already
    updated by the previous run.

`build/` is already gitignored (`.gitignore:13`); the generated header and
audit log never get committed.

## 6. Out of scope

- `touchLongPressAction` — gated by `FREEINK_CAP_TOUCH` from the SDK, a
  different mechanism. No change.
- `freeink-sdk` changes.
- Default enum values — unchanged on boards where a macro is 1.
- Stored JSON keys — unchanged.

## 7. Verification

1. **Generator:** `python3 scripts/gen_board_features.py --show` prints the
   dump table and the resolved flags for the default env; per-env headers
   spot-checked (`x4pro` → `HOME_KEY=1, MENU_BUTTON=0`).
2. **Host unit tests:** `cmake -S test -B build && cmake --build build &&
   ctest --test-dir build`.
3. **Firmware builds:** `pio run -e default|x4pro|sticky|papermono` —
   resolved flags `default` 0/1, `x4pro` 1/0, `sticky` 0/1, `papermono` 0/0.
   (Sticky's GT911 touch config has no home key in `BoardConfig.h`, so its
   flags differ from the pre-implementation prediction.)
4. **Device (X4 Pro):** flash, then in the EPUB reader: single Home press →
   go back one level; no Dictionary Word Select; double-press → configured
   double-click action; hold ≥700 ms → configured long-press action.

## 8. Risk

Low. The offending reader dispatchers and the `main.cpp` fall-through were
already deleted; this run adds the macro gates and the DRY generator chain
around what stays. The menu-button feature is preserved (re-added under a
gate). CI builds each env, which re-runs the generator and would catch any
device-flag mismatch.
