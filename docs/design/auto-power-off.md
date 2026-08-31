# Auto Power Off — post-sleep dwell timer & rail-drop shutdown (design)

> Feature: a user-configurable dwell timer that runs **after** the device enters deep
> sleep. Each deep-sleep session arms a fresh N-hour RTC timer; if it elapses while the
> device is still asleep, the device wakes on the RTC timer, draws a shutdown screen
> (current book cover, or the logo fallback), drops the master peripheral rail
> (X4 Pro `power.latch0` = GPIO1 driven LOW + pad hold), and re-enters a
> power-button-only deep sleep. The next power-button press boots normally.
>
> Related: [stock-firmware-power-architecture.md](stock-firmware-power-architecture.md)
> (OEM power topology), [battery-drain-stock-parity.md](battery-drain-stock-parity.md).
> OEM reference: xteink_app v7.4.4 exposes "Auto power off" (Options: Off/2/4/6/8/12 h,
> default 4 h per the decompiled settings table).
>
> All `file:line` citations are relative to the repo root unless prefixed `freeink-sdk/`
> (submodule at `cea1f1580137b2c7298f29dfdfb3016a20360753`).

---

## 0. Spec deviations flagged up-front (evidence-based)

1. **There is no "Power" settings category.** The device Settings UI has exactly four
   tabs — Display / Reader / Controls / System (`src/activities/settings/SettingsActivity.cpp:37-38`).
   "Time to Sleep" lives in **System** (`src/SettingsList.h:362-365` declares its row with
   `StrId::STR_CAT_SYSTEM`). Auto Power Off is therefore placed in **System**,
   directly **under** the "Time to Sleep" row, instead of a nonexistent "Power" tab.
2. **The picker MUST mirror `openSleepTimeoutPicker()` — a numeric `IntervalSelectionActivity`
   slider, NOT the enum OptionPopup.** Consistency with "Time to Sleep" is a hard
   requirement: the user explicitly chose the same UX. The Off/2h/4h/6h/8h/12h set is
   presented on a numeric hours slider (min 2, max 12, step 2) with **"Off" as the
   max-boundary label** (`maxBoundaryLabelId`), exactly mirroring auto-sleep's `"Never"`
   at `SLEEP_TIMEOUT_NEVER_MINUTES` (`CrossPointSettings.h:389-391`; `settingValueText`
   `SettingsActivity.cpp:491-500`). This means the new setting is a numeric
   `SettingInfo::Value` storing hours (like `sleepTimeoutMinutes`), not an enum (see §2, §6).
   Side effect of a slider: 10 h is also reachable between 8 h and 12 h (Off) — flag for
   product sign-off (the discrete set listed in §0 excludes it).
3. **True 0 µA is not achievable on X4 Pro and is not required.** There is no PMIC
   hard-off on this board: the maximum software power-off is dropping the master
   peripheral rail by driving `power.latch0` (GPIO1) LOW and holding the pad
   (`freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:1595-1602`,
   `:651-654` — "Releasing the pins later is a software power-off"). The ESP32-S3 RTC
   domain stays powered (it must, to detect EXT1 wake on GPIO3,
   `BoardConfig.h:1518-1521`). Sleep current = chip deep-sleep floor, not 0.
4. **`getWakeupReason()` cannot classify a timer wake as PowerButton.**
   `lib/hal/HalGPIO.cpp:271-292` maps `ESP_SLEEP_WAKEUP_GPIO|EXT1` → `PowerButton`
   (`:277-280`); an `ESP_SLEEP_WAKEUP_TIMER` cause falls through to
   `WakeupReason::Other` (`:291`) and would boot to Home. The design intercepts the
   timer wake explicitly in `setup()` (§3) rather than overloading the enum.

---

## 1. State machine

```text
                     manual power off
   idle ── power hold ≥ POWER_BUTTON_HOLD_MS ────────► enterPowerOff()
   (main loop)        main.cpp:898-908                main.cpp:460-484
                                                        ├─ stageAutoPowerOffCover(false) :462
                                                        ├─ render shutdown screen (§7) :465
                                                        ├─ APP_STATE.saveToFile() :470
                                                        ├─ Storage.remove(SLEEP_FRAME_FILE) :473
                                                        └─ enterPowerOffSleep(gpio) :483

                        any sleep trigger
   idle ─────────────────────────────────────────────► enterDeepSleep()
   (main loop)   auto-timeout      main.cpp:884-890    main.cpp:408-454
                 short-press SLEEP main.cpp:916-921      ├─ stageAutoPowerOffCover(true) :423
                 HOME_ACT_SLEEP    main.cpp:248-249      ├─ APP_STATE.saveToFile()  :425
                                                         ├─ activityManager.goToSleep() :430
                                                         ├─ display.deepSleep()     :447
                                                         └─ startDeepSleep(gpio, timerUs) :454
                                                                │
                                        HalPowerManager.cpp:135 │ (ARM RTC TIMER here, §5)
                                                                ▼
                                              deep sleep  [GPIO1 held HIGH :158-177,
                                                           gated rails cut :186,
                                                           Frontlight.park() :196-198,
                                                           deepSleepUntilPowerButton() :226]
                                                    │
              ┌─────────────────────────────────────┴──────────────────────────────┐
        EXT1 wake (power button GPIO3, ANY_LOW)                    TIMER wake (RTC)
        HalGPIO.cpp:277-280 → PowerButton                    esp_sleep_get_wakeup_cause()
              │                                                     │
              ▼                                                     ▼
        existing setup() routing                       intercept in setup()
        main.cpp (resume/splash/reader)                main.cpp:677-684
                                                              │
                                                              ▼
                                              render shutdown screen (§7)
                                              framed cover or Logo120 + caption
                                                              │
                                                              ▼
                                              HalPowerManager::enterPowerOffSleep()
                                              GPIO1 driven LOW + gpio_hold_en (§4)
                                              gated rails cut, EXT1 armed, deepSleep
                                                              │
                                                              ▼
                                              power-button-only deep sleep
              ┌─────────────────────────────────────────────────────┘
              ▼
        next power press → EXT1 wake → normal boot
        (BoardConfig::holdPowerRails() main.cpp:533 re-asserts GPIO1 HIGH;
         wake reason is still PowerButton, HalGPIO.cpp:277-280, so the existing
         resume routing opens the last book as usual)
```

The power-off lane never arms the RTC timer (`enterPowerOffSleep` disables TIMER
wakeup, `HalPowerManager.cpp:266`), so the shutdown screen stays on the panel
until the user presses power — the next press is an EXT1 wake that reads as a
normal cold boot. `stageAutoPowerOffCover(false)` in `enterPowerOff` shares the
cover-staging helper with `enterDeepSleep` (`main.cpp:397-405`); the `requireTimerEnabled`
flag is false there because the shutdown screen renders immediately, unconditionally.

Key invariant honored by the brief's constraint (4): **no third timer in `loop()`**.
The inactivity auto-sleep at `main.cpp:884-890` remains the only main-loop timer; the
dwell deadline lives entirely in the ESP32-S3 RTC timer and only exists while asleep.

---

## 2. Setting: enum, default, persistence, i18n

### 2.1 Numeric hours field (`src/CrossPointSettings.h`)

A plain numeric `uint8_t` storing the dwell time in **hours**, mirroring
`sleepTimeoutMinutes` (`:305`). "Off" is the **max-boundary sentinel** (12), exactly
like auto-sleep's `SLEEP_TIMEOUT_NEVER_MINUTES = 31` (`:389-391`). Constants are
`constexpr` per the AGENTS.md `constexpr`-first rule:

```cpp
// Auto power-off dwell timer (hours). 12 == Off (max-boundary sentinel,
// mirrors SLEEP_TIMEOUT_NEVER_MINUTES). Range enforced by the slider: 2..12 step 2.
static constexpr uint8_t AUTO_POWER_OFF_MIN_HOURS = 2;
static constexpr uint8_t AUTO_POWER_OFF_MAX_HOURS = 12;   // == Off
static constexpr uint8_t AUTO_POWER_OFF_STEP_HOURS = 2;
static constexpr uint8_t AUTO_POWER_OFF_DEFAULT_HOURS = 4;
```

Field + accessor (next to `sleepTimeoutMinutes` at `:305`; `getSleepTimeoutMs()`
declared at `:485`):

```cpp
uint8_t autoPowerOffHours = AUTO_POWER_OFF_DEFAULT_HOURS;   // "Auto power off" dwell timer (hours)

// 0 when Off (== AUTO_POWER_OFF_MAX_HOURS); otherwise the dwell time in milliseconds.
uint32_t getAutoPowerOffMs() const;
```

Implementation in `src/CrossPointSettings.cpp`, modeled on `getSleepTimeoutMs()`
(`:352-357`):

```cpp
uint32_t CrossPointSettings::getAutoPowerOffMs() const {
  if (autoPowerOffHours >= AUTO_POWER_OFF_MAX_HOURS) return 0;  // Off or corrupt -> disabled
  return static_cast<uint32_t>(autoPowerOffHours) * 3600UL * 1000UL;  // 12 h max, fits uint32
}
```

No separate setter: the settings UI writes `autoPowerOffHours` directly via the
`SettingInfo::Value` valuePtr (§6), like `sleepTimeoutMinutes`.

### 2.2 Persistence (`/.crosspoint/settings.json`)

`CrossPointSettings::getFilePath()` already returns `"/.crosspoint/settings.json"`
(`src/CrossPointSettings.h:455`). Adding the field to the `SettingInfo` table (§6)
makes the **generic** `toJson`/`fromJson` loops persist it automatically (same path as
`sleepTimeoutMinutes`):

- Write: generic loop `doc[info.key] = s.*(info.valuePtr)` (`src/CrossPointSettings.cpp:64-114`).
- Read: generic loop with the clamp lambda (`:116-270`, clamp at `:120`) — a numeric
  `VALUE` is clamped to `[min, max]` (here `[2, 12]`), and the
  `doc[key] | fieldDefault` pattern supplies `AUTO_POWER_OFF_DEFAULT_HOURS` (4) when the
  key is missing.
  **Migration**: a `settings.json` without `autoPowerOffHours` (every existing
  install) loads as **4 h (default)** with no extra code and no `needsResave` dance —
  unlike the legacy `sleepTimeout` migration which needed special handling
  (`:183-188`) because it changed *type*. A corrupt/zero value clamps to the min (2 h),
  identical to how an out-of-range `sleepTimeoutMinutes` clamps to 1 min.

### 2.3 i18n (`lib/I18n/translations/english.yaml`, 517 lines)

New `STR_*` keys (all user-facing text must go through `tr()` per AGENTS.md). The
slider reuses auto-sleep's value-format + max-boundary-label pattern, so only three
new keys are needed (no per-hour enum strings):

| Key | English value | Used by |
|---|---|---|
| `STR_AUTO_POWER_OFF` | `"Auto power off"` (OEM v7.4.4 casing) | Settings row title |
| `STR_AUTO_POWER_OFF_HOURS_FORMAT` | `"%u h"` | Slider readout + row value (`settingValueText`) |
| `STR_SHUTDOWN_PRESS_POWER` | `"The reader is shut down. Press the power button to start."` | Shutdown screen caption |

- **Off** label: reuse the existing `STR_STATE_OFF` (`"Off"`, `english.yaml:271`), the
  same string auto-sleep's `"Never"` neighbor uses and the enum-option pattern already
  uses elsewhere (e.g. `touchReaderControls` at `src/SettingsList.h:319-322`). It is
  passed as `maxBoundaryLabelId` to the `IntervalSelectionActivity`, so 12 h renders as
  "Off".
- **Caption**: two sentences, period-terminated, no em dash (e-ink font has no arrow
  glyphs; em dash also avoided for consistency). Mirrors the plain prose style of
  `STR_SLEEPING` (`english.yaml:8`).
- Existing neighbor strings for reference: `STR_TIME_TO_SLEEP` (`:137`),
  `STR_SLEEPING` (`:8`), `STR_SLEEP_TIMER_VALUE_FORMAT` (`:417`, `"%u min"`),
  `STR_SLEEP_NEVER` (`:418`, `"Never"`).

Workflow: add keys to `english.yaml` (reference) and seed the other
`lib/I18n/translations/*.yaml` or let them fall back to English; run
`python scripts/gen_i18n.py lib/I18n/translations lib/I18n/`. The three generated
files (`I18nKeys.h`, `I18nStrings.h`, `I18nStrings.cpp`) are gitignored — commit the
YAML sources only (AGENTS.md, "Generated Files").

---

## 3. Wake-cause routing (timer-wake vs button-wake)

**Where the intercept lives:** `src/main.cpp` `setup()`, inserted immediately after
`setupDisplayAndFonts(resume != Splash)` (`:618`) and **before** the `BootResume`
handling (`:625-652`) and activity routing (`:661-688`). At that point everything the
shutdown screen needs already exists and nothing stale has been drawn:

- `Storage.begin()` done (`:534`), `powerManager.flushSleepTrace()` done (`:544`)
- `APP_STATE.loadFromFile()` (`:548`) and `SETTINGS.loadFromFile()` (`:564`) done —
  so the persisted cover path (§7) and the setting are available
- `Frontlight.begin(...)` (`:575-576`) done — its `releaseOnWake()` has already
  cleared stale pad holds unconditionally (§8)
- display + fonts up (`:618`, fonts inserted `:458-471`)

**Why before `:625`:** the `BootResume::SplashlessWake` block (`:625-652`) loads
`/.crosspoint/sleep_frame.bin` (`SLEEP_FRAME_FILE`, `main.cpp:366`) and redraws the
previous session's sleep screen. A timer wake must not do that — the shutdown screen
replaces the session. The intercept returns from `setup()` without reaching routing,
so `isSleepWake`/`isPersistedSleepWake` (`:549-550`) and the reader-resume logic
(`:676-688`) are never evaluated for a timer wake.

**The check (all three conditions, firmware-side, no SDK change):**

```cpp
// main.cpp setup(), after setupDisplayAndFonts (:618)
if (esp_reset_reason() == ESP_RST_DEEPSLEEP &&
    esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER &&
    SETTINGS.getAutoPowerOffMs() > 0) {          // belt & braces: timer only armed when enabled (§5)
  SleepActivity::renderShutdownScreen(renderer); // §7 — never enters the reader activity
  removeSleepFrameBuffer();                      // delete stale SLEEP_FRAME_FILE (:366, remover at :375-387)
  powerManager.enterPowerOffSleep(gpio);         // §4 — [[noreturn]]
}
```

- `esp_reset_reason() == ESP_RST_DEEPSLEEP` is the deep-sleep-survival test already
  used at `lib/hal/HalPowerManager.cpp:283`; `esp_sleep_get_wakeup_cause()` is already
  read at `lib/hal/HalPowerManager.cpp:307` and `lib/hal/HalGPIO.cpp:272` (idempotent
  register read; `logSleepBattery()` in `powerManager.begin()` at `main.cpp:508` only
  logs it).
- Distinguishing: `ESP_SLEEP_WAKEUP_TIMER` = dwell deadline elapsed (shutdown path);
  `ESP_SLEEP_WAKEUP_EXT1` = power button (`HalGPIO.cpp:277-280` → normal boot/resume,
  unchanged).
- **The shutdown screen is rendered without re-entering the reader**: the intercept
  renders directly and never calls `goToReader`/`goHome` (`:661-688`). The reader
  state itself is untouched — `APP_STATE.openEpubPath` survives on SD, so the *next*
  button boot resumes the book through the existing guard chain
  (`:676-688`: `lastSleepFromReader` + `readerActivityLoadCount` boot-loop guard).
- Note `wakeupReason` was already consumed at `main.cpp:510`; a timer wake yields
  `WakeupReason::Other` (`HalGPIO.cpp:291`) which today would boot normally — the
  intercept is what converts "Other" into the shutdown path, before the
  `wakeupReason` switch (`:578-602`) and resume resolution (`:612-614`) can act on it.

---

## 4. Rail-drop shutdown sink

New sink `HalPowerManager::enterPowerOffSleep(HalGPIO& gpio)` in
`lib/hal/HalPowerManager.{h,cpp}`, modeled line-for-line on `startDeepSleep()`
(`lib/hal/HalPowerManager.cpp:135-226`) but **holding GPIO1 LOW instead of HIGH**:

```cpp
// lib/hal/HalPowerManager.cpp
[[noreturn]] void HalPowerManager::enterPowerOffSleep(HalGPIO& gpio) {
#if defined(ENABLE_SERIAL_LOG)
  logSerial.end();                                   // parity with startDeepSleep :136-142
#endif
  freeink::PowerManager::powerDownRailsForSleep();   // parity :186 — SD GPIO5 HIGH (active-low off),
                                                     // touch GPIO2 HIGH, EPD RST
                                                     // (freeink-sdk PowerManager.cpp:73-91)
#if FREEINK_FRONTLIGHT_LS
  Frontlight.park();                                 // parity :196-198 — pads LOW + gpio_hold_en
                                                     // (freeink-sdk FrontlightManager.cpp:326-356)
#endif
  // Drop the MASTER peripheral rail last: same loop shape as startDeepSleep :158-177
  // and freeink holdRailOff (freeink-sdk PowerManager.cpp:63-70). gpio_hold_dis first —
  // a pad held from the previous sleep cycle silently ignores drive writes
  // (comment at :159-167 and freeink-sdk PowerManager.cpp:60-62).
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0) continue;
    const gpio_num_t g = static_cast<gpio_num_t>(pin);
    gpio_hold_dis(g);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);                          // rail OFF (startDeepSleep holds HIGH, :160-176)
    gpio_hold_en(g);
  }
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);  // button-only re-sleep (§5)
  freeink::PowerManager::deepSleepUntilPowerButton();       // freeink-sdk PowerManager.cpp:101-105
}
```

Ordering rationale:

1. **Gated rails before master rail.** `powerDownRailsForSleep()` powers down SD
   (GPIO5 driven HIGH — the enable is active-low, `freeink-sdk BoardConfig.h:1508-1514`),
   touch (GPIO2 HIGH — active-low, `:1531-1560`) and the EPD reset line, each with the
   same `hold_dis → drive → hold_en` pattern (`freeink-sdk PowerManager.cpp:73-91`,
   helper at `:63-70`). Doing this while the master rail (GPIO1) is still HIGH keeps
   the sequencing identical to `startDeepSleep()` (`HalPowerManager.cpp:186` runs with
   GPIO1 held at `:158-177`).
2. **GPIO1 LOW last.** The pad hold applied here survives `esp_deep_sleep_start()`
   because `deepSleep()` calls `esp_sleep_config_gpio_isolate(); gpio_deep_sleep_hold_en();`
   (`freeink-sdk PowerManager.cpp:93-99`) — holds registered *before* the isolate+hold_en
   are the ones that stick; this is exactly how the existing GPIO1-HIGH fast-wake hold
   works (`HalPowerManager.cpp:158-177` and its comment `:159-167`).
3. **`deepSleepUntilPowerButton()`** (`freeink-sdk PowerManager.cpp:101-105`) =
   `waitForPowerButtonRelease()` (`:47-57`) + `armPowerButtonWakeup()` (`:36-45`,
   EXT1 `ANY_LOW` on GPIO3 — the power button is active-low with pull-up,
   `freeink-sdk BoardConfig.h:1518-1521`) + `deepSleep()`. The release-wait also makes
   the degenerate case safe: if the user happens to be holding power when the timer
   fires, the device waits for release and only then sleeps, so the armed EXT1
   `ANY_LOW` cannot fire immediately.
4. **What is deliberately NOT copied from `startDeepSleep()`:**
   - the C3-only GPIO13 battery-latch block (`:144-156`, compiled only under
     `#if !SOC_PM_SUPPORT_EXT1_WAKEUP`) — Auto Power Off targets X4 Pro (S3); on C3
     boards without `power.latch0/latch1` the loop above no-ops and the sink
     degenerates to a plain button-only sleep after the shutdown screen (documented
     graceful degradation);
   - the PaperMono PMIC `requestShutdown()` block (`:200-207`) — X4 Pro has no PMIC
     hard-off (§0.3);
   - the RTC-timer arming (§5) — a shutdown sleep must **not** re-arm it, hence the
     explicit `esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER)`.

Reused sinks, per brief constraint (3): `Frontlight.park()`, `powerDownRailsForSleep()`,
`esp_sleep_config_gpio_isolate()` (inside `deepSleep()`) — the only delta is the latch
level (LOW vs HIGH).

### 4.1 Manual power off (`enterPowerOff`)

The same rail-drop shutdown sink is also the sink for the manual gesture: a
power-button **hold** in the main loop (`main.cpp:898-908`) now calls
`enterPowerOff()` (`main.cpp:460-484`) instead of `enterDeepSleep()`. The hold
threshold is the constant `POWER_BUTTON_HOLD_MS = 400` (`CrossPointSettings.h:418-419`) —
the same value the SDK uses to discriminate a shared Confirm/Power hold
(`freeink-sdk InputManager.h:427` `CONFIRM_POWER_HOLD_MS`) and the same ceiling the
`PWR_CONFIRM` click detector always used for non-Sleep bindings
(`MappedInputManager.cpp:313`). A short power click never reaches the hold path; with
the Sleep binding (now the default, `CrossPointSettings.h:281-284`) it sleeps via the
release-based path at `main.cpp:916-921`.

`enterPowerOff()` shares the sleep screen with the timer-wake path, minus the wake
round-trip:

1. `stageAutoPowerOffCover(false)` (`main.cpp:462`) — the same cover-staging helper
   `enterDeepSleep()` uses (`main.cpp:397-405`), but with `requireTimerEnabled=false`:
   the shutdown screen renders immediately, so the cover is staged unconditionally
   (an open book gets its cover; no book falls back to Logo120, §7.2).
2. `SleepActivity::renderShutdownScreen(renderer)` under a `RenderLock`
   (`main.cpp:465`) paints the shutdown screen *before* sleeping — no timer-wake
   intercept in `setup()` is involved on this path (that intercept, `main.cpp:677-684`,
   only handles wakes from an auto-power-off dwell).
3. Persist the post-off state: `APP_STATE.showBootScreen = false` +
   `APP_STATE.saveToFile()` (`main.cpp:469-470`) so the next power press boots
   splashless, and `Storage.remove(SLEEP_FRAME_FILE)` (`main.cpp:473`) so a stale
   Quick Resume frame cannot replace the shutdown screen on the next boot.
4. WiFi teardown identical to `enterDeepSleep()` (`main.cpp:477-480`), then
   `powerManager.enterPowerOffSleep(gpio)` (`main.cpp:483`, the §4 sink — TIMER
   wakeup explicitly disabled at `HalPowerManager.cpp:266`).

Because the hold path never arms the RTC timer, the shutdown screen stays on the
panel indefinitely at deep-sleep current; the next power press is an EXT1 wake that
reads as a normal cold boot (§1 power-off lane). `enterPowerOff()` deliberately does
*not* latch `deepSleepInProgress` or run `activityManager.goToSleep()`: no activity
teardown is needed when the process is about to die, and the shutdown screen has
already been painted.

---

## 5. RTC-timer arming (location, gating, wake-cause read)

**Location:** `lib/hal/HalPowerManager.cpp` `startDeepSleep()`, just before the
`deepSleepUntilPowerButton()` call (`:226`) — i.e. any time before
`esp_deep_sleep_start()` (`freeink-sdk PowerManager.cpp:93-99`). The wakeup config
persists across the intervening rail/park calls; `esp_sleep_config_gpio_isolate()`
does not cancel it.

**Signature change (firmware-only):**

```cpp
// lib/hal/HalPowerManager.h
void startDeepSleep(HalGPIO& gpio, uint64_t autoPowerOffTimerUs = 0);
```

```cpp
// lib/hal/HalPowerManager.cpp, in startDeepSleep() before :226
if (autoPowerOffTimerUs > 0) {
  esp_sleep_enable_timer_wakeup(autoPowerOffTimerUs);  // RTC timer, runs in deep sleep
}
```

**Gating at the call site** — `main.cpp::enterDeepSleep()` (`:408-454`), the funnel
for *all* user-facing sleep triggers (auto-timeout `:884-890`, short-press SLEEP
`:916-921` via `SETTINGS.shortPwrBtn`, and HOME_ACT_SLEEP `:248-249`, plus
quick-resume variants via `setSleepReason` `:418`):

```cpp
// main.cpp:450-454 (modified)
uint64_t autoPowerOffUs = 0;
if (const uint32_t apOffMs = SETTINGS.getAutoPowerOffMs(); apOffMs > 0) {
  autoPowerOffUs = static_cast<uint64_t>(apOffMs) * 1000ULL;  // 12 h max = 4.32e10 µs, fits uint64
}
powerManager.startDeepSleep(gpio, autoPowerOffUs);
```

The power-button **hold** is *not* in this funnel: it is now the manual power-off
gesture (`enterPowerOff`, `main.cpp:898-908` → `:460-484`). The hold threshold is the
constant `POWER_BUTTON_HOLD_MS = 400` (`CrossPointSettings.h:418-419`), matching the
SDK's own `CONFIRM_POWER_HOLD_MS` (`freeink-sdk InputManager.h:427`) and the
`PWR_CONFIRM` click ceiling (`MappedInputManager.cpp:313`); a short power click
instead sleeps via the release-based path above.

Properties:

- **Per-session countdown, no accumulator.** `esp_sleep_enable_timer_wakeup()` is
  re-armed with the full duration on every entry; elapsed sleep time is never added
  up and never persisted (the brief's "fresh N-hour timer" per session). The
  countdown restarts if the user wakes the device for even one page turn.
- **Setting Off → timer never armed.** The default parameter (`0`) keeps the two
  direct `startDeepSleep()` call sites timer-free: the ghost-wake re-sleep at
  `main.cpp:511-514` (wake verified as spurious) and the `AfterUSBPower` sleep on
  non-X4Pro boards (`:594`). Documented deviation: at `:511-514` settings are not
  yet loaded (`SETTINGS.loadFromFile()` is at `:564`), and that path is a
  continuation of a just-interrupted session, not a user sleep action.
- **Wake-cause read** happens on the *next* boot via
  `esp_sleep_get_wakeup_cause()` (`ESP_SLEEP_WAKEUP_TIMER` vs `ESP_SLEEP_WAKEUP_EXT1`),
  as wired in §3. `ESP_SLEEP_WAKEUP_TIMER` is already a recognized value in this
  codebase (`lib/hal/HalPowerManager.cpp:307,375`).
- **Not an already-shutdown sleep:** the shutdown sink (§4) explicitly disarms the
  timer source before sleeping, satisfying constraint (4)'s "not already a shutdown".

---

## 6. Settings UI

**Row registration** — `src/SettingsList.h`, inserted immediately after the
`STR_TIME_TO_SLEEP` row (`:362-365`) inside the `STR_CAT_SYSTEM` block (`:361-375`).
It is a numeric `SettingInfo::Value`, exactly like the auto-sleep row it sits under:

```cpp
SettingInfo::Value(
    StrId::STR_AUTO_POWER_OFF, &CrossPointSettings::autoPowerOffHours,
    {CrossPointSettings::AUTO_POWER_OFF_MIN_HOURS, CrossPointSettings::AUTO_POWER_OFF_MAX_HOURS,
     CrossPointSettings::AUTO_POWER_OFF_STEP_HOURS},
    "autoPowerOffHours", StrId::STR_CAT_SYSTEM),
```

Row order = declaration order in `rebuildSettingsLists()`
(`src/activities/settings/SettingsActivity.cpp:43-130` filters `getSettingsList()` by
category preserving order), so "Auto power off" lands **directly under** "Time to
Sleep" on the System tab.

**Picker — mirror `openSleepTimeoutPicker()`** (`SettingsActivity.cpp:446-459`). Add
`openAutoPowerOffPicker()` that launches the same `IntervalSelectionActivity`, with the
hours range + `"Off"` max-boundary label:

```cpp
void SettingsActivity::openAutoPowerOffPicker() {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "AutoPowerOffInterval", StrId::STR_AUTO_POWER_OFF,
          SETTINGS.autoPowerOffHours,
          CrossPointSettings::AUTO_POWER_OFF_MIN_HOURS,
          CrossPointSettings::AUTO_POWER_OFF_MAX_HOURS,
          CrossPointSettings::AUTO_POWER_OFF_STEP_HOURS,
          CrossPointSettings::AUTO_POWER_OFF_STEP_HOURS,
          StrId::STR_AUTO_POWER_OFF_HOURS_FORMAT, false, StrId::STR_STATE_OFF),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          SETTINGS.autoPowerOffHours = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}
```

**Dispatch** — in `toggleCurrentSetting()` (`SettingsActivity.cpp:274-421`), add the
`STR_AUTO_POWER_OFF` branch next to the `STR_TIME_TO_SLEEP` one (`:284-287`) so it
calls `openAutoPowerOffPicker()` and returns. This is the same ~3-line dispatch
auto-sleep uses; no new generic path.

**Row value text** — extend `settingValueText()`'s `STR_TIME_TO_SLEEP` branch
(`SettingsActivity.cpp:491-500`) to also handle `STR_AUTO_POWER_OFF`: show
`STR_STATE_OFF` when `autoPowerOffHours >= AUTO_POWER_OFF_MAX_HOURS`, else format with
`STR_AUTO_POWER_OFF_HOURS_FORMAT` (e.g. `"4 h"`).

---

## 7. Shutdown screen + book-cover path persistence

### 7.1 Cover path persisted to `APP_STATE` at sleep time

New field in `src/CrossPointState.h` (fields block `:14-25`):

```cpp
std::string autoPowerOffCoverBmpPath;  // resolved cover BMP for the shutdown screen
```

Wired into the existing JSON round-trip (`src/CrossPointState.cpp`):

- `toJson` (`:43-56`): `doc["autoPowerOffCoverBmpPath"] = autoPowerOffCoverBmpPath;`
- `fromJson` (`:58-94`): `autoPowerOffCoverBmpPath = doc["autoPowerOffCoverBmpPath"] | "";`
  (same pattern as `openEpubPath` at `:59`). Missing key → empty → Logo120 fallback.

**Population point:** `main.cpp::enterDeepSleep()`, before `APP_STATE.saveToFile()`
at `:405` (so it rides the save that already happens on every sleep entry — no extra
SD write; SD persistence throttling per AGENTS.md). The path derivation logic is
factored out of `SleepActivity::renderCoverSleepScreen()` (`:750-827` — XTC `:769-782`,
TXT `:783-796`, EPUB `:797-811`, each ending in a `getCoverBmpPath()` after
`generateCoverBmp()`) into a shared public static:

```cpp
// SleepActivity.h — reused by renderCoverSleepScreen (:765-814) and sleep entry
// Returns true and fills outPath when a cover BMP exists (generating it if needed);
// false when the book has no cover / format unsupported.
static bool resolveCoverBmpPath(const std::string& bookPath, std::string& outPath);
```

```cpp
// main.cpp::enterDeepSleep(), before APP_STATE.saveToFile() :405
APP_STATE.autoPowerOffCoverBmpPath.clear();
if (SETTINGS.getAutoPowerOffMs() > 0 && !APP_STATE.openEpubPath.empty()) {
  std::string coverPath;
  if (SleepActivity::resolveCoverBmpPath(APP_STATE.openEpubPath, coverPath)) {
    APP_STATE.autoPowerOffCoverBmpPath = std::move(coverPath);
  }
}
```

Notes:

- This runs regardless of `SETTINGS.sleepScreen` mode, because the shutdown screen
  shows the book cover even when the user's sleep screen is Dark/Custom — that is why
  it lives in `enterDeepSleep()` rather than inside `renderCoverSleepScreen()`
  (which only executes for COVER/COVER_CUSTOM modes, `SleepActivity.cpp:534-549`).
- EPUB derivation is metadata-only (`lastEpub.load(true, true)` skips CSS,
  `SleepActivity.cpp:797-811`) and `generateCoverBmp()` output is cached under
  `.crosspoint/`, so the per-sleep cost is a cache hit in the common case.
- The buffer/decode discipline of `SleepActivity` is reused at render time, not here:
  heap work happens with `makeUniqueNoThrow` (`SleepActivity.cpp:332,418,462`) and
  SD font caches are released before big decodes (`releaseSdFontCachesForDecode`,
  `:480-486`).

### 7.2 Shutdown screen render

New public static `SleepActivity::renderShutdownScreen(GfxRenderer&)` (declared in
`src/activities/boot_sleep/SleepActivity.h`, next to the ctor at `:11-12`),
implemented in `SleepActivity.cpp` reusing the existing helpers:

```cpp
void SleepActivity::renderShutdownScreen(GfxRenderer& renderer) {
  renderer.clearScreen();
  const bool haveCover = !APP_STATE.autoPowerOffCoverBmpPath.empty();
  if (haveCover) {
    HalFile f;
    haveCover = Storage.openFileForRead("Shutdown", APP_STATE.autoPowerOffCoverBmpPath.c_str(), f) &&
                bitmap.parseHeaders(f);                       // pattern: :816-824
  }
  if (haveCover) {
    // Frame ≈ 60–70 % of the oriented panel, centered; the cover is placed inside the
    // frame with the existing fit-and-center placement math (calculateBitmapPlacement,
    // :122-152, honoring SETTINGS.sleepScreenCoverMode CROP) — no new scaler.
    drawFrameRect(...);                                       // UITheme-derived metrics
    renderBitmapSleepScreen(bitmap, /*preserveBackground=*/true);  // :612-662 pipeline
  } else {
    renderer.drawImage(Logo120, centered 120x120);            // parity with :600
  }
  drawFrame border + caption:
    renderer.drawCenteredText(UI_10_FONT_ID, ..., tr(STR_SHUTDOWN_PRESS_POWER));  // :601-602 pattern
  display.setInverted(false);                                 // sleep screens are normal polarity, :498
  display.displayBuffer(HALF_REFRESH);                        // single-update parity, :609 (:591-594 comment)
  APP_STATE.autoPowerOffCoverBmpPath.clear();                 // consumed; next sleep repopulates
}
```

Design points:

- **Frame size:** target rectangle = ~60–70 % of `renderer.getScreenWidth()` /
  `getScreenHeight()` (orientation-aware; never hardcode 800×480, AGENTS.md UI rule),
  centered within `renderer.getOrientedViewableTRBL()`. The existing placement helper
  (`calculateBitmapPlacement`, `:122-152`) needs a small extension to accept a bounding
  rect (today it centers on the full page); it already fits-and-centers oversized
  bitmaps and honors CROP, which yields "cover inside a frame" without adding a bitmap
  scaler. Frame border drawn with UITheme primitives (GUI macro rule) — same visual
  family as the popup frame in `drawSleepPopupPreservingFrame` (`:452-478`).
- **Fallback:** framed `Logo120` + the same caption, mirroring
  `renderDefaultSleepScreen` (`:595-610`: `drawImage(Logo120)` `:600`, centered text
  `:601-602`) with `STR_SHUTDOWN_PRESS_POWER` replacing `STR_SLEEPING` (`:602`).
- **Single refresh:** one `displayBuffer(HALF_REFRESH)` (`:609`; comment `:591-594`
  documents why sleep screens use a single 0xD7 update). No grayscale LSB/MSB passes
  needed for a monochrome frame+caption (those are for dithered photos,
  `renderBitmapSleepScreen` `:612-662` / `tryRenderTransparentOverlayBmp` `:327-369`).
- **No reader, no activity:** called from `setup()` (§3), not pushed onto the
  ActivityManager stack; nothing is allocated beyond the render-time buffers, and
  `onEnter()/onExit()` lifecycle rules don't apply.
- The consumed path is cleared after render; `enterPowerOffSleep()` (§4) then drops
  the rail. The stale quick-resume frame file is removed by the intercept (§3) so the
  post-shutdown button boot starts clean (`loadSleepFrameBuffer` would otherwise draw
  the previous session's sleep screen at `main.cpp:625-652`).

---

## 8. Risks & deep-sleep-survival pitfalls

1. **The `_lsParked` rule — pad holds survive, DRAM flags do not.**
   `freeink-sdk FrontlightManager.cpp:362-370` documents the canonical bug:
   `park()` latches `gpio_hold_en` on the frontlight pads, which survives deep sleep
   *and the wake reset*, while `_lsParked` is a plain DRAM flag that resets to `false`
   — gating the wake-side `releaseOnWake()` on the flag would leave pads held forever.
   Consequence for this feature: the shutdown path must never gate a pad release or a
   re-init on a DRAM variable ("we parked", "we shut down"). Everything acting after
   wake is either unconditional (pad `gpio_hold_dis`, as `FrontlightManager::begin`
   already does at `:173-190`) or derived from sources that survive deep sleep:
   `esp_reset_reason()` (`HalPowerManager.cpp:283`), `esp_sleep_get_wakeup_cause()`
   (`:307`), `RTC_DATA_ATTR` state with a version byte (`:47-77`), `rtc_time_get()`
   tick deltas (`:280-301`), or SD-persisted `APP_STATE`. The design introduces **no
   new DRAM flag** for "shutdown happened" — the wake cause *is* the state.
2. **Timer wake is invisible to `getWakeupReason()`** (`HalGPIO.cpp:291` → `Other`):
   if the intercept (§3) were placed after the resume routing, a timer wake would boot
   to Home and silently defeat the feature, and worse, the device would then sit
   awake. The intercept's position after `:618` / before `:625` is load-bearing.
3. **`flushSleepTrace()` will log the timer wake as "spurious".** The CSV row computes
   `spurious = (wakeCause != ESP_SLEEP_WAKEUP_EXT1)` (`lib/hal/HalPowerManager.cpp:375`)
   and runs at `main.cpp:544`, before the intercept. A timer wake writes a spurious
   row. Optional minimal follow-up (firmware-side, allowed): treat
   `ESP_SLEEP_WAKEUP_TIMER` as non-spurious when the last sleep armed the timer
   (add an `RTC_DATA_ATTR` bool set in `startDeepSleep`, version-bump
   `SLEEP_TRACE_VERSION` per `:47-77`). Not required for correctness.
4. **Rail-drop is not power-off.** GPIO1 LOW kills the panel/SD/touch rails
   (`BoardConfig.h:1595-1601` — the panel rail *and* the SD slot depend on it), but
   the S3 RTC domain + EXT1 detector stay powered. Expect OEM-parity sleep current,
   not 0 µA (§0.3). Human-test item: measure with the stock-firmware comparison
   methodology from `battery-drain-stock-parity.md`.
5. **The boot after a shutdown is a full boot, not a seamless quick-resume.** GPIO1
   was dropped, so the fast-wake property bought by holding it HIGH
   (`HalPowerManager.cpp:158-177`, PR #3215 context in `FrontlightManager.cpp:327-335`)
   is gone for that session; `BoardConfig::holdPowerRails()` (`main.cpp:480`,
   `freeink-sdk BoardConfig.h:1901`) re-asserts it during the next boot. Wake reason is
   still EXT1 → PowerButton (`HalGPIO.cpp:277-280`), so resume routing
   (`main.cpp:676-688`) reopens the book normally.
6. **Held power button at timer fire** is safe: `waitForPowerButtonRelease()`
   (`freeink-sdk PowerManager.cpp:47-57`) blocks the shutdown sleep until release, so
   the armed EXT1 `ANY_LOW` cannot instantly re-wake (`:36-45`, `:101-105`).
7. **USB charging during auto-power-off** still ends with rails dropped; charging
   circuitry is independent of `power.latch0` (`chargeEnable`,
   `freeink-sdk BoardConfig.h:655-666`). Flag for human test: verify charge-while-off
   behaves like stock's auto power off.
8. **Cover BMP missing/corrupt at shutdown** (SD swapped, book deleted while asleep):
   `openFileForRead`/`parseHeaders` failure → Logo120 fallback (`:826` parity). No
   crash path; all render buffers via `makeUniqueNoThrow` with LOG_ERR fallback.
9. **Heap:** the shutdown render adds no persistent allocations; it reuses the
   `SleepActivity` render pipeline (`renderBitmapSleepScreen` `:612-662`) whose buffers
   are `makeUniqueNoThrow`-scoped (`:332`, `:462`). The only new resident memory is
   `std::string autoPowerOffCoverBmpPath` in `CrossPointState` (path-length string,
   SD-backed store, same class of field as `openEpubPath`, `CrossPointState.h:14-25`).
10. **Watchdog:** the shutdown sequence runs in `setup()` (render + two SD ops +
    rail drops), well under the 5 s loop-watchdog budget; the render path is the same
    one used by every sleep screen today.

---

## 9. freeink-sdk changes

**None required.** Everything lands in firmware:

| File | Change |
|---|---|
| `src/CrossPointSettings.h/.cpp` | `autoPowerOffHours` numeric field + constants, `getAutoPowerOffMs()` (§2) |
| `src/SettingsList.h` | one `SettingInfo::Value` row after `:365` (§6) |
| `src/CrossPointState.h/.cpp` | `autoPowerOffCoverBmpPath` round-trip (§7.1) |
| `src/main.cpp` | timer param at `:430`, cover-path populate before `:405`, wake intercept after `:618`, stale-frame removal (§3, §5, §7) |
| `lib/hal/HalPowerManager.h/.cpp` | `startDeepSleep(gpio, timerUs=0)` arming (§5); new `enterPowerOffSleep()` (§4) |
| `src/activities/boot_sleep/SleepActivity.h/.cpp` | `resolveCoverBmpPath()` (shared with `:765-814`), `renderShutdownScreen()` (§7) |
| `lib/I18n/translations/*.yaml` | 3 new `STR_*` keys (§2.3); Off reuses `STR_STATE_OFF` |

The one nominally SDK-shaped piece — driving a latch pin LOW + hold — is a 6-line
pattern already duplicated between `HalPowerManager.cpp:158-177` (HIGH) and
`freeink-sdk PowerManager.cpp:63-70` (`holdRailOff`); replicating it once more in
`lib/hal` avoids a submodule PR. If maintainers later want it shared, the exact SDK
change would be: **`freeink-sdk/libs/hardware/PowerManager/{include/PowerManager.h,src/PowerManager.cpp}`**
— add `[[noreturn]] void deepSleepUntilPowerButtonRailOff()` alongside
`deepSleepUntilPowerButton()` (`PowerManager.h:57`, `PowerManager.cpp:101-105`)
reusing `holdRailOff` (`:63-70`) with `power.latch*` LOW. Explicitly out of scope for
this feature.

---

## 10. Verification checklist

AI-verifiable: `pio run` (default env), `pio check`, `./bin/clang-format-fix -g`;
settings JSON round-trip via emulated build if available; grep that no `ESP_SLEEP_WAKEUP_TIMER`
path reaches activity routing.

Human/device (flagged): 2h setting → sleep → confirm shutdown screen at ~2 h; power
press → normal boot into last book; Off setting → no timer wake; all sleep triggers
arm the timer; manual power hold (~0.5 s) → shutdown screen, no boot loop afterwards,
next power press → normal boot; short power click → sleep (Sleep binding default;
frontlight double-click on the power button is unreachable under this binding,
unchanged from the previous build); Paper Mono short click →
sleep; sleep-trace CSV row semantics (§8.3); frontlight dark after shutdown;
charge-while-off (§8.7); all 4 orientations for the framed cover.
