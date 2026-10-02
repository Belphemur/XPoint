# Crossfire Feature Port — Endurance Governor + Power Stats Overlay

Date: 2026-10-01
Status: Draft for review
Base: Crossfire BETA v1.11 (release `X4pro`, `Crossfire-x4-pro-1.6.0-crossfire.11.bin`)
      — a binary-only fork of CrossInk 1.6.0 — ported into XPoint `develop`.
Evidence source: Ghidra decompilation of the decrypted firmware image and the
CrossInk 1.6.0 open-source base. Decompiles live in
`~/workspace/eink/stock-firmware/crossfire/ghidra_outputs/`.

## 1. What Crossfire adds to CrossInk

Crossfire extends CrossInk's `lib/hal/HalPowerManager.cpp` in two directions and
both additions are what the X4 Pro device screenshots show:

1. **Endurance governor** — a FreeRTOS task that owns CPU clock switching and
   light-sleep entry, with a persisted crash-safety ladder.
2. **Power Stats overlay** — a settings-gated on-screen telemetry block
   (Compact = one status line, Full = ~20-row block).

The port keeps both features but reorders the layering to fit XPoint's HAL
conventions (HAL classes over SDK, `tr()` for all UI strings, no raw owning
pointers). Nothing in the port raises the C3 floor.

## 2. The 80 MHz idle floor — is it a real constraint?

**No. It is not a hardware constraint on the ESP32-S3. We can go to 10 MHz idle.**

### 2.1 Clock topology facts (verified online 2026-10-01)

| Fact | Source |
|------|--------|
| CPU/C3/S3 `setCpuFrequencyMhz()` floor = **10 MHz** (40 MHz XTAL divider chain — 240/160/80 are the "all XTAL" values, 40/20/10 are 40 MHz-XTAL-only) | Arduino-ESP32 `esp32-hal-cpu.c` documentation comments, mirrored widely |
| On the ESP32-S3 **PSRAM clock is derived via MSPI, not from the CPU core clock** | Espressif IDF "SPI Flash and External SPI RAM Configuration" (ESP32-S3): "On ESP32-S3, MSPI stands for the SPI0/1… The main flash and PSRAM are connected to the MSPI peripheral. CPU accesses them via Cache." MSPI DDR/SDR frequency is selected separately (`CONFIG_SPIRAM_SPEED`, `CONFIG_ESPTOOLPY_FLASHFREQ`) from CPU ownership. |
| On the ESP32-S3 **the APB/CPU clocks are independent from MSPI's** | Same doc's mode/speed tables: PSRAM-only windows run at 40 MHz DDR / 80 MHz DDR / 120 MHz DDR on F4R8/F8R8, all achievable regardless of CPU MHz. Lowering the CPU therefore does not slow or destabilize PSRAM on this SoC. |
| Note that DOES apply, but only to LRT (light-sleep-while-cached) | Espressif IDF "Sleep Modes": "Since the sleep process disables the cache, the task requesting to enter sleep must have its stack located in internal RAM. Tasks with their stacks in PSRAM are not allowed to request sleep." This applies to the light-sleep REQUESTING task, not to PSRAM itself — and the X4 Pro already gates this on the GT911 idle-sleep path. |

### 2.2 CrossInk's floor is conservative, not physical

CrossInk (crossink v1.5.0, `lib/hal/HalPowerManager.h:43-47`) picks:

```cpp
#if defined(BOARD_HAS_PSRAM)
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
```

That inverts the physical situation (the C3 board has no PSRAM at all and still
runs fine at 10 MHz), so the 80 MHz value is a conservative default, not a
PSRAM tolerance limit.

### 2.3 Crossfire's own proof

Crossfire's release notes and its decompiled overlay make 10 MHz idle a live,
shipping feature on this exact S3 + 8 MB PSRAM board:

- `Crossfire-BETA` release `X4pro`: "The chip idles at 10 MHz instead of stock's
  80 MHz and speeds up briefly for user-visible work." Plus: "the CPU drops to
  10 MHz while the screen is redrawing, since it has nothing else to do."
- The Power Stats overlay format string confirms this is the *default profile*
  (accessible in the UI, not a debug easter egg):
  `rest %s%uMHz nap %s%% | %u.%02uV %u.%u%% | ~%smA | pg %lums` — screenshot shows
  it reading `rest 80MHz nap 2.1%`.
- The Endurance governor's ready log includes a safety ladder
  (`Endurance governor ready: boot at %d MHz, safety=%u`) — Crossfire does not
  trust that 10 MHz is risk-free either; the ladder is what makes it shippable.

### 2.4 Residual, non-PSRAM risks with 10 MHz

These are the actual mechanisms to watch, none of them PSRAM-related:

1. **Interrupt latency** — IDF documents DFS adding up to ~40 µs interrupt-entry
   latency when a 40/10 MHz→80 MHz switch happens on the interrupt path; on the
   ESP32-S3 the range shifts slightly but the mechanism is the same. This is a
   budget by default, not a bug — unless a subsystem assumes tighter latency than
   that. The X4 Pro's GT911/touch ISR currently fires from a poll task — check
   the poll cadence the governor inherits before entering deep idle.
2. **WiFi** — `esp_wifi_start()` holds `ESP_PM_APB_FREQ_MAX` for the whole radio
   life (per-idle release only when modem-sleep is enabled). It therefore forces
   the CPU high regardless of the governor's target. Port the "Wi-Fi leak guard"
   alongside the governor (Crossfire has one) so a stray `esp_wifi_start()` is
   not silently holding the clock high forever.
3. **Xtensa boot/config minimums** — some Arduino/ESP-IDF SDK rules implicitly
   assume 80 MHz+ (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` is a compile-time constant
   here). Explicitly re-read the XPoint sdkconfig for rules grounded in CPU MHz
   the governor must interact with, before trusting `setCpuFrequencyMhz(10)`.
4. **The e-ink panel's SPI bus** — NOT the CPU: the X4 Pro's UC8279 driver
   defaults its panel SPI to 10 MHz across the Xteink board profile (see the
   `FREEINK_UC8279X4_SPI_HZ` opt-in in a personal CrossInk fork's PR #3).
   Port that as an XPoint opt-in flag if the port needs faster AA page turns —
   but note the risk (UC8279 is rated to 20 MHz but Xteink board-profile batches
   were only validated at 10 MHz).

## 3. Design: ported components

Three sources for everything below: crossink v1.6.0 base (`HalPowerManager`),
Crossfire's decompiled governors (stored as `FUN_4205a430`, governor init;
`FUN_4205a390`, profile application; `FUN_42059b34`, safety migration), and the
decompiled overlay composer `FUN_421d6228`.

### 3.1 `EnduranceGovernor` (Class, `lib/power/EnduranceGovernor.h/.cpp` new)

A HAL-adjacent class that wraps the existing `HalPowerManager`'s clock
switch (the existing `setPowerSaving(bool)` funnel is kept — the governor now
owns its *target* instead of the existing 80 MHz binary state).

**Identity & persistence**

- `powerProfile`: `Endurance` (default) / `Balanced` / `Performance`
  — the Crossfire default confirms "Endurance" on first boot.
- `idleClockMHz`, `renderClockMHz` (per-profile defaults; the Settings UI
  exposes them as fabrication already shipped by Crossfire).
- NVS key `endurance.state` `u8`:
  `bit0 = idle Strikes >= 1, bit1 = lightSleep Strikes >= 1` (Crossfire
  migrates `0x%02x` → `0x%02x` — matches the decompiled `Migrated endurance.2
  safety state 0x%02x -> 0x%02x`).

**Clock ladder** (from decompiled `Profile %u: idle %d / active %d / burst %d /
render %u MHz, light sleep %s` and the strike-migration state machine)

```
strategy 0 – idle 10 MHz + light sleep enabled (fully Endurance)
strategy 1 – idle raised (40 MHz) + light sleep enabled
strategy 2 – idle raised (40 MHz), no light sleep
strategy 3 – idle 80 MHz, no light sleep  (CrossInk's stock floor)
```

**Crash-strike migration**

- Boot: read `endurance.state`; for each struck dimension, promote at least one
  strategy level; `LOG_INF` the old→new state byte and clear the strikes.
- During runtime: the governor's escalation task (see below) increments the
  strike counters on detected instability; a `esp_restart()`-class crash
  re-runs the increment on next boot; the strike byte is what makes 10 MHz
  self-healing.

**Escalation task**

- FreeRTOS task, pinned to core 0 (per the fork's `kCore` worker convention),
  small stack (Crossfire's version creates this at
  `Governor task creation failed; on-demand escalation disabled` — a 2048-byte
  stack is enough).
- Runtime role: when a job that needs >10 MHz appears (page render, WiFi
  activation), the governor pre-raises the clock and holds it; once the job
  finishes, the clock returns to the profile's idle target.
- Inputs: an `EnduranceGovernor::Lock` RAII class (the existing
  `HalPowerManager::Lock` pattern is kept; the governor honours it), and a
  "heavy-job" request queue.

**Touch-INT wake verification**

From the decompiled strings: `Touch INT verified as a light-sleep wake source`,
`Touch INT missed wakes; using %lu ms poll slices`, `Touch INT wakes without
touches; using poll slices`. Runtime procedure:
1. On first light-sleep entry, arm the touch INT as a wake source and see if
   it fires cleanly.
2. If misses/ghost wakes are detected, the governor demotes itself to
   **poll-slice mode**: no full light-sleep; instead, X ms poll windows where
   the CPU stays at the profile's idle clock. Crossfire's default is 50 ms.
3. The verification result is cached (the decompiled
  `Touch INT verified as a light-sleep wake source` is a one-time
  boot-time log).

### 3.2 `PowerDrainMonitor` (Class, `lib/power/PowerDrainMonitor.h/.cpp` new)

The measurement backend behind the overlay's drain line. Formula extracted
from the decompiled overlay composer (`FUN_421d6228`:

```
SoC_before        ← mAh remaining from BatteryMonitor (BatteryMonitor.readPercentage())
SoC_after         ← ditto, one minute later (or at wake)
%/h               = (SoC_before - SoC_after) * 3600 / Δt_seconds
avg_mA            = %/h * assumed_capacity_mAh / 100
assumed_capacity_mAh = 1100   (the constant in Crossfire's binary: fStack_bc * 1100.0)
runtime_left      = remaining_% * assumed_capacity_mAh / avg_mA
```

The assumption of 1100 mAh is Crossfire's, not ours — check the X4 Pro's
actual pack capacity before shipping and make it a constant in
`lib/power/PowerDrainMonitor.h` documented as "assumed pack capacity".

**10-minute window**

Crossfire only shows a drain figure after a 10-minute measurement window
(decompiled: `Drain: measuring (%lu/10 min)`) — before that the overlay shows
`measuring (N/10 min)` instead of a number. Keep that GATE; an instantaneous
reading is noisy on an e-ink reader whose current draw is 5–30 mA depending on
page-turn state.

**Per-sleep rate**

The monitor also keeps a per-sleep %/h (decompiled:
`Last sleep %s -%ld.%02ld%% %lu.%03lu%%/h`, `Last sleep %s (too short to
rate)`) — sleep-duration < threshold shows "too short to rate" instead of a
divide-by-zero. Gate the rate line behind that check.

### 3.3 `PowerStatsOverlay` (Class, `src/activities/settings` new, `lib/power/PowerStatsOverlay.h` shared)

The UI surface. Two modes (matching the decompiled `param_5 = (uint)((uint)fStack_200 - 2U & 0xff)` trim window where `param_5` < 2 = Compact mode):

- **Compact** = one line in the status bar:
  `rest 80MHz nap 2.1% | 4.21V 95.8% | ~32.8mA | pg 694ms`
  (decompiled format: `rest %s%uMHz nap %s%% | %u.%02uV %u.%u%% | ~%smA | pg %lums`).
- **Full** = text block overlay, up to 20 rows × 0x30 bytes
  ( decompiled:
  `ENDURANCE  %s  boost:%s`, `Rest %s%u MHz  (last %s)`,
  ` nap %s%% %uM %s%% %uM %s%% %uM %s%%`, `Held up:`,
  `Clocks %u/%u/%u render %u nap:%s`, `Since boot nap %s%% idle %s%%`,
  `Naps %lu wake btn %lu tch %lu tmr %lu`, `Batt %u.%03uV %u.%02u%% %s`,
  `Drain: n/a on USB power`, `Drain %lu.%02lu%%/h = %smA meas`,
  `Drain: measuring (%lu/10 min)`, `Est avg %smA  left ~%s`,
  `Last sleep %s -%ld.%02ld%% %lu.%03lu%%/h`,
  `Last sleep %s (too short to rate)`,
  `Page %lums = %lu render + %lu panel`,
  `Refresh F%lu H%lu P%lu G%lu wave %lums`,
  `Heap %luK min %luK  PSRAM %...`, `Radio %s sess %lu on %lus guard %lu`,
  `Light %s %u%% Touch %s`, `Chip %dC up %s clk %lu %s`)

**Rendering strategy** — the decompiled composer draws to the framebuffer
directly (inherits the fork's existing renderer's conventions). In XPoint the
port follows the same principles as the existing status-bar overlay:
- `tr()`-translated UI strings.
- Do not run a timer that forces periodic redraws (e-ink wear); instead,
  re-render the overlay only when a page turn/new data event happens.
- Fold in the `Nap/wake counter` accounting on the governor's
  light-sleep entry/exit PRs (#3032/#3055) hooks.

**Settings Activity** — a `POWER` section tab in the existing `SettingsListActivity`
pattern (and a `PowerStatsOverlay` row in the `StatusBarSettingsActivity`
pattern): Off / Compact / Full (the same enum-extended pattern as
`references/i18n-and-gestures.md`).

### 3.4 Wi-Fi leak guard

A small utility (not a class — a `lib/power/` RAII wrapper for `esp_wifi_start`
/ `esp_wifi_stop` accounting). Crossfire's release notes confirm:
"Wi-Fi leak guard: Wi-Fi is switched off if something leaves it on by mistake."
This pairs with the governor's WiFi-forces-CPU-high interaction (§2.4 #2).

## 4. Verification plan

Per XPoint's per-phase hardware gate:

1. **Host first**: unit-test the drain math (PSRAM-free host pattern per
   `references/host-test-lib-pattern.md`) — the formula in §3.2 is
   deterministic; test the 10-min gate, the "too short to rate" guard, and the
   runtime-left string builder.
2. **Static**: `pio check -e default --fail-on-defect low --fail-on-defect
   medium --fail-on-defect high` on the fork's develop (Crossfire's addition
   does not raise the C3 floor — the whole `lib/power/` tree is target-agnostic
   and `poolMalloc`-free).
3. **On device**: flash a PR binary, profile through the existing
   `scripts/debugging_monitor.py`:
   - Boot at 10 MHz: `Endurance governor ready: boot at 10 MHz, safety=0`.
   - Verify page turns raise the clock: page render at render-clock (80 MHz);
     overlay Full mode's `Page %lums = %lu render + %lu panel` line reports
     both halves.
   - Sleep/wake: wake-source counters in the overlay increment (btn / tch / tmr).
   - Crash ladder: force a `esp_restart()` (e.g. battery-removal sim), re-boot
     and confirm the `Migrated endurance.X safety state 0x00 -> 0x01` log shows
     the strike byte, the governor auto-demoted at least one strategy level,
     and the next run stays at a safer clock.
4. **CI**: release.yml's per-device build for `x4pro` (and `default` as a
   floor) compile.

## 5. Out of scope

- Fixing CrossInk's conservatism (§2.2's 80 MHz-on-PSRAM default) in CrossInk
  itself — that is a CrossInk upstream conversation, not a port item.
- Changing the panel-driver's 10 MHz SPI default — that is a freeink-sdk
  conversation (opt-in flag only, per §2.4 #4).
- Porting Crossfire's *Speed* set features (50 ms key-press page turn, sleep
  image caching, 240 MHz page drawing, 30-page cleanup interval) — those touch
  the render pipeline and deserve their own design pass.

## 6. Open items

1. Confirm the actual X4 Pro pack capacity (the 1100 mAh constant is
   Crossfire's assumption, not a datasheet value).
2. Confirm `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` / XPoint `sdkconfig` for other
   CPU-MHz grounded rules the governor must interact with (§2.4 #3).
3. Decide whether the Full overlay's row count budget (20 rows × 0x30 bytes)
   saturates the fork's existing status-bar/overlay machinery memory budget.
4. Decide the escalate-task stack and priority against the existing
   `FibpPrefetchWorker` / `ProgressManager` core-0 family (§3.1's role).

## 7. Addendum — overlay geometry and the 10 MHz SD failure (2026-10-02)

### 7.1 Overlay placement

Two rules govern where the telemetry may be painted, and they are not in tension
once stated precisely:

- **Compact** shares the reader's status-bar band with the clock. It is *formatted*
  by `PowerStatsOverlay::buildCompact()` and *laid out* by
  `BaseTheme::drawStatusBar()` as a first-class cluster element, because that
  function owns the baseline (`src/components/themes/BaseTheme.cpp:831`) and has
  already painted the battery/progress clusters on it. The band itself moves: an
  XTC reader set to `XTC_STATUS_BAR_TOP` passes a computed `paddingBottom` that
  resolves the same expression to `orientedMarginTop`, so Compact follows the bar
  to whichever edge the user chose.
- **Full** is anchored at the top (`y = marginTop + 4 + bandAbovePx`). It is not
  an overlay over the text: reflowable readers subtract `topReservePx()` from their
  content top margin, so the layout viewport and the paint origin move together
  (`EpubReaderActivity.cpp:2256`, `TtfBookRuntime.cpp:461`, and the two hit-test
  sites at `:980`/`:1014` that must agree with the stored layout).

The reservation is a **constant**, `kFullRowCount` rows, which is why every row in
`drawFull()` is emitted unconditionally with a placeholder when its data is absent.
A data-dependent height would change `viewportHeight` as telemetry arrived, and
`viewportHeight` is part of the section cache key
(`Section.cpp:164-165`, compared on load at `:228`) — the book would re-paginate
mid-read. `drawFull()` logs an error if the built row count ever drifts from the
reserved count.

XTC pages are pre-rendered bitmaps and cannot reflow, so on that reader the block
overlaps the page image; when the XTC bar is also Top-positioned, the block is
pushed below that band so the two never fight.

### 7.2 The 10 MHz SD-card failure

Field log at the idle rung:

```
E (13031) sdmmc_cmd: sdmmc_read_sectors_dma: sdmmc_send_cmd returned 0x107, failed to get status (0x107)
E (14039) sdmmc_req: handle_idle_state_events unhandled: 00001000 00000000
[SD] Failed to open file for writing: /.crosspoint/state.json.tmp
```

The first hypotheses to rule out, because they are wrong for this firmware:

- **It is not light sleep.** There is no light sleep anywhere in this tree —
  `grep -rn 'esp_light_sleep|light_sleep|CONFIG_PM_ENABLE|pm_lock' src/ lib/
  freeink-sdk/libs` returns nothing. ESP-IDF's own SDMMC power-management lock is
  gated on `CONFIG_PM_ENABLE` (`components/esp_driver_sdmmc/src/sdmmc_transaction.c:185-187`)
  and is therefore compiled out here.
- **It is not the card clock.** SD is wired as native 4-bit SDMMC
  (`SdmmcBlockDevice.cpp:47`, `sdmmc_host_t host = SDMMC_HOST_DEFAULT()`), and
  `SDMMC_CLK_SRC_DEFAULT = SOC_MOD_CLK_PLL_F160M`
  (`components/soc/esp32s3/include/soc/clk_tree_defs.h:481`). The bus clock is
  derived from the fixed 160 MHz PLL, so lowering the CPU does not slow SCLK.

What is left is the host side. The failing call is
`sdmmc_write_sectors_dma` (`components/sdmmc/sdmmc_cmd.c:551`): the multi-block
write command returns `ESP_ERR_TIMEOUT` **and** the recovery `CMD13` status query
times out too — that is the `failed to get status (0x107)` branch at
`sdmmc_cmd.c:597`. Losing both means command *sequencing* broke, not that the card
was merely slow. The follow-on `handle_idle_state_events unhandled: 00001000` is
the driver objecting that a non-card-detect interrupt status survived into idle,
which its own comment states cannot happen
(`sdmmc_transaction.c:193-207`).

The mechanism: the SD host's register interface and completion ISR are paced by
APB, and APB follows the CPU frequency. The governor changes that divider from a
core-0 task (`EnduranceGovernor::escalationTaskEntry()` → `applyStrategy()`) and
from `HalPowerManager::setPowerSaving()` on whichever task runs it — asynchronously
to the task that is mid-transfer. A divider retune in that window drops the
completion event.

The fix is to make the two mutually exclusive, at the funnel every SD access in
this firmware already passes through:

- `HalStorage::StorageLock` (the recursive `storageMutex` holder used by all 22
  storage entry points, including `HalFile::Impl::~Impl`) marks the bus busy for
  the duration via `sd_bus::TransactionCounter` (`lib/hal/SdBusGuard.h`), and on
  the 0 → 1 edge raises the clock *before* any command is issued.
- `HalPowerManager::setPowerSaving()` refuses a drop while
  `HalStorage::transactionActive()` (`HalPowerManager.cpp:97`), alongside the
  existing "Wi-Fi is up" refusal.

Only the counter edges act, so nesting is free and the guard sits above the
backend — the C3 (SPI SD) and the S3 (SDMMC) hold identically. The accounting is
host-tested in `test/power/PowerTest.cpp` (`SdBusGuard.*`); the clock refusal
itself is on-device only.

Trade-off worth stating plainly: this keeps the CPU out of the idle rung during SD
I/O, so a page render that reads many small sections costs clock raises. That is
the correct direction — a save that silently corrupts is not a saving.
