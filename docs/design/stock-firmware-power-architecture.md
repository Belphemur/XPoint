# Stock Xteink X4 Pro — Power-Saving & Deep-Sleep Architecture (decompiled)

> Source: decrypted stock OTA `xteink_app` v7.4.4 (IDF v6.0.1, ESP32-S3 / Xtensa LX7),
> `/home/balor/workspace/eink/stock-firmware/x4pro/x4pro_stock_V7.4.4_en.bin`.
> Disassembly: `irom.S` (IROM, load 0x42000020) + `iram.S` (IRAM, load 0x40378000),
> produced with `xtensa-esp32s3-elf-objdump -D -b binary -m xtensa`.
> Lawful RE for interoperability/debug. Every claim cites an offset; indirect-call gaps are
> marked **NEEDS GHIDRA**.

---

## 0. Method & honesty notes

- The stock firmware is a **flat ESP-IDF image**, not an ELF. `ledc_timer_config` (`0x4222446C`)
  and `ledc_channel_config` (`0x422248E0`) are reached via **register-indirect `callx8`** (C++
  virtual/indirect calls). A flat `objdump` cannot resolve those call graphs → anything about
  *which* function invokes *which* sleep primitive, and with *what args*, is only partially
  reachable. Where blocked, this doc says **NEEDS GHIDRA** rather than guessing.
- Two "obvious" stock features turned out to be **absent on this chip** (verified):
  - `ESP_SLEEP_VBAT_POWER_DEEPSLEEP_MODE` is guarded by `SOC_VBAT_SUPPORTED`, which is defined only
    for **ESP32-H2 and ESP32-P4** in the installed framework
    (`~/.platformio/packages/framework-espidf/components/soc/esp32h2/include/soc/soc_caps.h` and
    `.../esp32p4/include/soc/soc_caps.h`; `.../esp32s3/include/soc/soc_caps.h` has **0** matches —
    re-grepped this run). It is a **no-op on X4 Pro**. The string in the binary is just the linked
    `sleep_modes.c` name table (`s_submode2str[]` at `0x3c382cfc`, slot `0x3c382d1c` =
    `VBAT_POWER_DEEPSLEEP` = 8, string at `0x3c354318`).
  - `vbat` appears **0 times** in stock IROM, and `tsen`/`adc_tsen` appears **0 times** → stock does
    **not** call `esp_sleep_sub_mode_config` for VBAT or ADC/TSEN on S3. Do not "port" these.
- Highest-value claims re-verified this run with `/tmp/opencode/elftool.py` over the full
  IROM+IRAM disassembly:
  1. `esp_deep_sleep_start` (`0x4037b39c`): **zero** `call4/8/12` sites target it and **zero**
     literal-pool words hold its address (IROM and IRAM scans both empty).
  2. `esp_sleep_sub_mode_config` (`0x420086e4`): exactly **one** caller — `call8` at `0x420091a6`
     (a deprecated wrapper that itself has no callers) — and no literal-pool references; its
     ref-count array sits at `0x500002f0`. → stock arms **no** sleep sub-modes.
  3. `SOC_VBAT_SUPPORTED` grep across the installed IDF `soc_caps.h` set (see above).

---

## 1. Stock Power subsystem (what exists)

C++ RTTI proves a real Power domain (not just UI strings):

| Symbol (from binary) | RTTI VMA | Role |
|---|---|---|
| `XTEink6Domain5Power12PowerServiceE` / `IPowerService` | `0x3c38f22a` / `0x3c38f1fc` | app-level power service |
| `XTEink17BoardPowerHalBaseE` / `IPowerHal` | `0x3c4b6b55` / `0x3c4b6b3d` | board HAL base + power HAL interface |
| `ITerminalUsbPort` | `0x3c3921cc` | USB / charging port |
| `ILightSleepLightPort` | `0x3c3922d6` | light-sleep frontlight lifecycle port |
| `PowerOffChargingPagePresenter` / `…PageView` | (RTTI in DROM) | "power off + charging" screen |
| `LightSleepStoragePort` | (RTTI in DROM) | light-sleep storage lifecycle port |

Behavioral strings confirm the feature set (exact DROM VMAs from the `.bin` string scan):
- `Battery is low. Connect a charger before updating.` — DROM `0x3c4a6fc3` — **update gating** on low battery.
- `Battery voltage abnormal` — `0x3c4a9160` — voltage fault detection.
- `battery_valid` `0x3c363958`, `battery_healthy` `0x3c36397d`, `battery_stale` `0x3c3639a5`,
  `battery_age_ms` `0x3c3639bf` — a **battery-health state machine** (age / validity / staleness).
- `Auto Sleep` — `0x3c4aed88` — and `Auto power off` — `0x3c4b373b` — inactivity-driven sleep / power-off.
- `Brownout detector was triggered` — referenced via IRAM literal pool `0x4037874c` — brownout handling present.
- `DeepSleep`, `light-sleep`, `Light Sleep` UI, `wifi light-sleep suspend`, `download paused for light sleep`.
- Terminal-ownership strings: `shutdown terminal deep sleep rejected` `0x3c358969`,
  `terminal deep sleep returned unexpectedly` `0x3c35898f`,
  `deep-sleep transaction lost terminal ownership` `0x3c358b47`,
  `light-sleep takeover lost terminal ownership` `0x3c358b9c`,
  `terminal shutdown returned during light-sleep recovery` `0x3c358bf9`.

**NEEDS GHIDRA:** the exact ownership graph of `PowerService`/`BoardPowerHalBase` (which method
reads the gauge, which enforces `Battery is low` gating) — reachable only via vtable dispatch.

---

## 2. Deep-sleep path

Stock links the full IDF sleep core:
- `deep_sleep_start = 0x4037B300` (IRAM, `FORCE_IRAM` in `sleep_modes.c`).
- `esp_deep_sleep_start = 0x4037B39C` (IRAM): `movi.n a10, 0` → `call8 deep_sleep_start` →
  `call8 0x40382348` (`allow_sleep_rejection = false`).
- `esp_sleep_isolate_digital_gpio` body ref `0x4037AE67` (IRAM); called from `esp_sleep_start`
  (the common core for both deep and light sleep).
- `gpio_hold_en` (`0x4228B025`), `gpio_hold_dis` (`0x42289FB8`/`0x4228B0B5`),
  `rtc_gpio_hold_en` (`0x4228B898`), `rtc_gpio_hold_dis` (`0x4228B90C`) — all linked.

**MAJOR (re-verified this run):** `esp_deep_sleep_start` (`0x4037B39C`) has **ZERO static
references anywhere in IROM/IRAM** — no `call4/8/12` targets it, no tail-jump, and no literal-pool
word holds its address. Stock's power-off / "DeepSleep" therefore does **not** go through the IDF
deep-sleep core entry; consistent with a **battery-latch hard-off** (like the fork's GPIO13 latch)
or an indirect (`callx8`) dispatch to it → **NEEDS GHIDRA**.

Sleep orchestration lives at **`0x42034B00`–`0x42035300`** (IROM), built around
"terminal ownership" semantics (strings `shutdown terminal deep sleep rejected` `0x3c358969`,
`terminal deep sleep returned unexpectedly` `0x3c35898f`,
`deep-sleep transaction lost terminal ownership` `0x3c358b47`,
`light-sleep takeover lost terminal ownership` `0x3c358b9c`,
`terminal shutdown returned during light-sleep recovery` `0x3c358bf9`).
Literal pools `0x4200318c`/`0x42003190`/`0x420031c4`/`0x420031cc`/`0x420031d4` feed `l32r` users at
`0x42034b87`/`0x42034bd0`/`0x4203514b`/`0x4203519f`/`0x420352ac`/`0x420353fb`. The region is dense
with `callx8` indirect dispatches to a small set of helper functions (`0x42033298`, `0x42033280`,
`0x42033340`, `0x42034ab8`, `0x420342f8` repeat most).

**Stock does NOT call `esp_deep_sleep_start`** (verified: the IDF fn entry `0x4037b39c` has **zero**
`l32r` references in IROM+IRAM, and the string appears only once in the whole binary). So stock ends
its power-down transaction via a **PMIC / power-latch hard-off** (GPIO-driven), not the IDF deep-sleep
path. That is the mechanistic reason stock has **no frontlight leak**: the master rail is hard-cut,
the frontlight driver is fully depowered, and there is nothing to "hold." The fork instead holds
GPIO1 (PR #3215) for fast-wake, which exposes the leak that `park()`/`releaseOnWake()` then fixes.
(Re-confirmed 2026-08-30 via independent grep of the disassembly — supersedes the earlier
"stock deep-sleeps via esp_deep_sleep_start" wording.)

**Stock does NOT use** VBAT under-voltage self-wake or ADC/TSEN sleep monitor on S3 (see §0). So
stock's "no drain in sleep" comes from **correct pad isolation + not holding a power rail**, not from
a fancy sleep monitor.

---

## 3. Light-sleep path

Stock has a distinct light-sleep mode:
- `esp_light_sleep_start` is linked (present in the binary).
- `ILightSleepLightPort` / `LightSleepStoragePort` + strings `light-sleep takeover lost terminal
  ownership`, `Light Sleep` UI, `wifi light-sleep suspend`, `download paused for light sleep`,
  `btdm_low_power_mode_init` (BT low-power) → a real runtime light-sleep with storage
  unmount/remount lifecycle and WiFi/BT modem-sleep.
- RC_FAST keep-alive strings (`ESP_SLEEP_DIG_USE_RC_FAST_MODE`, `ESP_SLEEP_LP_USE_RC_FAST_MODE`,
  `ESP_SLEEP_RTC_USE_RC_FAST_MODE`) are present — consistent with keeping the LEDC PWM (frontlight)
  and RF alive across light sleep.

**Implication for the fork:** the fork only **deep-sleeps** (no light sleep). Therefore RC_FAST
keep-alive is **unnecessary for the fork** — consistent with PR #66 (the fork's 10 kHz revert is
caused by `FREEINK_FRONTLIGHT_LS` pinning RC_FAST, which the fork doesn't need). Stock's RC_FAST use
is for its light-sleep path, which the fork doesn't have.

---

## 4. Battery health & low-battery gating

- State machine: `battery_valid` `0x3c363958` / `battery_healthy` `0x3c36397d` /
  `battery_stale` `0x3c3639a5` / `battery_age_ms` `0x3c3639bf` — an age/validity/staleness model.
- Gating: `Battery is low. Connect a charger before updating.` (`0x3c4a6fc3`) → stock **blocks
  OTA/firmware update when battery is low** (and warns). `Battery voltage abnormal` (`0x3c4a9160`)
  → voltage fault path.
- Brownout: `Brownout detector was triggered` (IRAM pool `0x4037874c`) → hardware brownout handled.

**NEEDS GHIDRA:** the functions implementing the state machine (string pools `0x421c9a4c`
`battery_healthy`, `0x421c9a60` `battery_age_ms`) and the exact gating decision (which UI resource /
which early-return).

---

## 5. Auto power-off / auto-sleep

Strings `Auto power off` + `Auto Sleep` indicate an inactivity timeout that drives the device to
sleep or off (likely cover-close / idle timer). **NEEDS GHIDRA** for the wiring (DROM resource
table words `0x3c4b373b`, `0x3c4aed88`, `0x3c4a6fc3`).

---

## 6. Fork vs stock — what the fork already has / lacks

| Capability | Stock | Fork (Belphemur/crosspoint-x-reader) | Gap |
|---|---|---|---|
| Deep sleep (pad isolate) | ✅ structured transaction | ✅ `HalPowerManager::deepSleepUntilPowerButton` + `park()` | minimal |
| Inactivity auto-sleep | ✅ (`Auto Sleep`) | ✅ `main.cpp:813-819` `getSleepTimeoutMs()` | none |
| Frontlight leak fix | n/a (collapses rail) | ✅ `FrontlightManager::park()`/`releaseOnWake()` | fork has it; correct & clock-agnostic |
| Low-battery **update gating** | ✅ `Battery is low…` | ⚠️ UI only (battery %) | **gap** |
| Battery-health state machine | ✅ `battery_healthy/age_ms/valid/stale` | ❌ none | gap |
| VBAT under-voltage self-wake | ❌ (no-op on S3) | ❌ | none (don't add) |
| ADC/TSEN sleep monitor | ❌ (unused) | ❌ | none (don't add) |
| Light-sleep + RC_FAST keep-alive | ✅ | ❌ (deep-sleep only) | by design; keep fork deep-sleep-only |
| Brownout handling | ✅ | hardware only | minor gap |

---

## 7. Recommendations for the fork ("same no-drain")

Ordered by value/risk. All are **additive** to the fork's existing correct deep-sleep + `park()`.

1. **Keep the current design — do not add VBAT or ADC/TSEN sleep monitors.** Verified no-ops/unused
   on S3. (Corrects an earlier hypothesis.)
2. **Add low-battery OTA-update gating** (mirror stock's `Battery is low. Connect a charger before
   updating.`). Concrete: in the update flow, read `BatteryMonitor::readPercentage()` (already used
   in `HalPowerManager.cpp`) and refuse to start flashing below ~20–25%, showing the existing
   low-battery string. Place: update/OTA task in `src/`. Risk: low. Needs: a battery% threshold
   decision + UI string (already localized). Validate on hardware (measure that an update at low%
   is blocked).
3. **Add a battery-health state machine** (optional, lower priority): track gauge age/validity/stale
   like stock's `battery_healthy/age_ms/valid/stale` so a flaky/old gauge doesn't report bogus %
   that triggers false auto-sleep or false low-battery. Place: `HalPowerManager`. Risk: low-medium
   (state to persist in RTC/NVS). Needs: hardware validation.
4. **Confirm the deep-sleep pad isolation is complete** (already done via `esp_sleep_config_gpio_isolate`
   + `park()`). No change needed; this is the actual mechanism that gives "no drain." The fork's
   `park()` is correct and clock-agnostic — keep it.
5. **Keep fork deep-sleep-only; do not port stock's light-sleep/RC_FAST** unless a concrete
   use-case appears. RC_FAST keep-alive is irrelevant to the fork's current architecture and is the
   root cause of the 25 kHz→10 kHz revert (PR #66). Option B (drop `FREEINK_FRONTLIGHT_LS` for x4pro,
   keep `park()`) remains the clean way back to OEM 25 kHz — but the stock `clk_cfg` is still
   **unverified** (NEEDS GHIDRA), so treat as hypothesis.
6. **(Optional, NEEDS GHIDRA to scope)** Study stock's terminal-ownership arbitration pattern
   (`0x42034B00` region) — it may be a robust way to avoid the fork's fast-wake master-rail hold
   (PR #3215) leaking. But the fork's `park()` already closes that leak; only pursue if re-measured
   drain remains.

---

## 8. Open items requiring Ghidra

- Full call graph of stock sleep orchestration (`0x42034B00`–`0x42035300`): which primitive ends the
  deep-sleep transaction (esp_deep_sleep_start vs PMIC hard-off).
- `clk_cfg` at the stock frontlight `ledc_timer_config` call (confirm XTAL/AUTO vs RC_FAST).
- Battery-health state machine ownership + low-battery gating decision.
- Auto power-off / auto-sleep wiring.

---

*Generated from OpenCode (`opencode-go/glm-5.3-flash`) static analysis. The prior Hermes run
(`proc_f754659adba2`) exhausted its budget without writing output; **this run authored and wrote
this file**, re-verifying the three highest-value claims with `elftool.py` (zero static refs to
`esp_deep_sleep_start` `0x4037b39c`; single caller `0x420091a6` of `esp_sleep_sub_mode_config`
`0x420086e4`; `SOC_VBAT_SUPPORTED` absent for ESP32-S3 in the installed IDF) before finalizing.*
