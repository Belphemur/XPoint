# Battery drain — match stock firmware power behavior

> Status: design doc (analysis done, 3 implementation tasks scoped below).
> Companion analysis: `stock-firmware-power-architecture.md` (decompiled from the real
> `xteink_app` v7.4.4 image; see `crosspoint-stock-firmware` skill for how to obtain/RE it).
> RE is lawful (interoperability/debug); the stock binary was downloaded from the vendor's own
> `/debug` / flash-tools API.

## TL;DR

The fork already has the parts that actually matter for "no drain": correct deep-sleep pad
isolation, an inactivity auto-sleep timeout, and the `park()`/`releaseOnWake()` frontlight-leak
fix. Two stock features are worth porting (low-battery update gating, battery-health state
machine); two commonly-assumed features are **not** applicable to ESP32-S3 (VBAT under-voltage
self-wake, ADC/TSEN sleep monitor — both no-ops/unused on S3, verified). Keep the fork
deep-sleep-only; do not port stock's light-sleep/RC_FAST path.

## What stock actually does (verified from binary)

| Capability | Stock | Fork today | Verified? |
|---|---|---|---|
| Deep-sleep pad isolation | structured terminal-ownership transaction (`0x42034B00`–`0x42035300`) | `HalPowerManager::deepSleepUntilPowerButton` + `esp_sleep_config_gpio_isolate` | ✅ |
| Inactivity auto-sleep | `Auto Sleep` string | `main.cpp:813-819` `getSleepTimeoutMs()` | ✅ |
| Frontlight leak fix | n/a (collapses master rail) | `FrontlightManager::park()`/`releaseOnWake()` | ✅ (correct, clock-agnostic) |
| Low-battery **update gating** | `Battery is low. Connect a charger before updating.` | UI % only | ⚠️ gap |
| Battery-health state machine | `battery_healthy`/`battery_age_ms`/`battery_valid`/`battery_stale` | none | ⚠️ gap |
| VBAT under-voltage self-wake | ❌ (no-op on S3) | ❌ | ✅ don't add |
| ADC/TSEN sleep monitor | ❌ (unused) | ❌ | ✅ don't add |
| Light-sleep + RC_FAST keep-alive | ✅ (`esp_light_sleep_start`, `ILightSleepLightPort`) | ❌ deep-sleep only | by design |

Corrected hypotheses (2026-08-30): earlier suggestions to port `ESP_SLEEP_VBAT_POWER_DEEPSLEEP_MODE`
and `ESP_SLEEP_USE_ADC_TSEN_MONITOR_MODE` were wrong — `SOC_VBAT_SUPPORTED` is not defined for S3
and both are unreferenced in stock IROM (`vbat`/`tsen` = 0 hits). Stock's "no drain" = isolation +
rail not held.

## Suggestion 1 — Low-battery OTA / firmware-update gating  [priority: high]

Mirror stock's `Battery is low. Connect a charger before updating.`

- **Where:** the OTA/update entry point in `src/` (firmware update task). The gauge is already
  readable via `BatteryMonitor` (used in `HalPowerManager.cpp`/`HalGPIO.cpp`).
- **What:** before starting a flash, read `BatteryMonitor::readPercentage()`; if `< ~20–25%`, abort
  with the existing low-battery string and do not proceed.
- **Risk:** low. Pure guard; no new hardware dependency.
- **Validate on hardware:** confirm an update initiated at low % is blocked and shows the warning.
- **Branch:** `feature/low-battery-update-gate` (worktree).

## Suggestion 2 — Battery-health state machine  [priority: medium]

Mirror stock's `battery_healthy`/`battery_age_ms`/`battery_valid`/`battery_stale` so a flaky/old
Coulomb gauge can't drive false low-battery or false auto-sleep.

- **Where:** `HalPowerManager.cpp` — add a small health struct persisted in RTC slow memory / NVS.
- **What:** track gauge age + validity/staleness flags; if gauge reports stale/invalid, fall back
  to a safe assumption (don't auto-sleep, don't block updates) and surface a diagnostic.
- **Risk:** low–medium (persistence + edge cases).
- **Validate on hardware:** force a stale reading (disconnect gauge I²C) → confirm no false action.
- **Branch:** `feature/battery-health-state` (worktree).

## Suggestion 3 — Confirm/lock the deep-sleep isolation + keep fork deep-sleep-only  [priority: keep]

- No code change required: `park()` + `esp_sleep_config_gpio_isolate` is the actual "no drain"
  mechanism. Add a regression check (assert no pad held except the intentional power-button latch)
  and a comment block citing this analysis.
- Explicitly **do not** port stock's light-sleep/RC_FAST path unless a use-case appears; RC_FAST is
  the root cause of the 25 kHz → 10 kHz revert (SDK PR #66). Option B (drop `FREEINK_FRONTLIGHT_LS`
  for x4pro, keep `park()`) stays the clean route back to OEM 25 kHz — but stock's `clk_cfg` is
  still unverified (NEEDS GHIDRA), so treat as hypothesis.
- **Branch:** folded into existing battery-drain worktree (no new branch), or `chore/sleep-isolation-doc`.

## Open (NEEDS GHIDRA)

Four items blocked by C++ indirect calls in flat disasm, being resolved by a parallel Ghidra task:
sleep-orchestration call graph, stock `clk_cfg`, health-state ownership, auto-sleep wiring.

## Conventions

- Branches: `feature/<name>` from `develop` (hard CI gate `CI Status` on develop/master).
- Commits: conventional-commit titles.
- PR is the deliverable; docs updated alongside code.
