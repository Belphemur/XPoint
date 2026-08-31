# Stock-parity shutdown-reason RTC marker — SDK primitive design

## Proven stock mechanism (ghidra_poweroff_report.md, FINAL CONCLUSION)
xteink_app v7.4.4 records WHY it powered off across the deep-sleep boundary in
**RTC slow RAM** (survives deep sleep + most resets):
- `0x50000004` (RTC slow RAM offset 4) <- magic `0x58435253` (bytes "SRCX")
- `0x50000000` (RTC slow RAM offset 0) <- u8 reason code
- Boot side: if magic present, reset reason = clean power-off with the recorded
  code, reading `(reason << 8) | 1`; both cells are cleared after the read.

## SDK primitive (freeink-sdk `PowerManager`)
Add to `freeink::PowerManager`:
- `static void setShutdownReason(uint8_t reason);` — writes magic+reason cells.
- `static uint16_t takeShutdownReason();` — returns `(reason<<8)|1` if magic
  matches (0 if not), and clears both cells. Idempotent: a second read returns 0.
- `enum ShutdownReason : uint8_t` with stock-honest values: `User = 1`
  (power-button long press / short-press off), `AutoOff = 2` (auto power off
  timer), `LowBattery = 3` (reserved, not yet wired).
- Direct `*(volatile uint32_t*)0x50000004` / `0x50000000` access — same
  mechanism as stock (RTC slow RAM is 8 KB at 0x50000000 on S3; offset 0/4 are
  user cells; the IDF does not own these in the Arduino build) — with a
  `SOC_RTC_SLOW_MEM` guard so non-S3 targets compile to no-ops (returns 0).
- S3-only guard `#if SOC_RTC_SLOW_MEM_SUPPORTED || SOC_RTC_SLOW_MEM` per the
  SDK's SoC-capability style; on unsupported targets setShutdownReason is a
  no-op and takeShutdownReason returns 0 — consumers must treat 0 as "no marker".

## Firmware consumer (crosspoint-x-reader)
- `enterPowerOff()` (manual long-press) -> `setShutdownReason(ShutdownReason::User)`
  before the sink.
- Timer-wake shutdown intercept (setup()) and the dwell arming in
  `enterDeepSleep()` -> `setShutdownReason(ShutdownReason::AutoOff)` when
  `getAutoPowerOffMs() > 0` — written at SLEEP ENTRY so the marker survives even
  if the timer wake itself crashes before re-writing (stock semantics).
- `setup()` early: `const uint16_t sr = takeShutdownReason(); if (sr) LOG_INF`
  + stash into the dev sleep-trace (a `reason` column already exists; add the
  value to the existing CSV row rather than a new column to keep the schema stable —
  actually add as new column `shutdown_reason` at the END to avoid breaking parsers).
- No board-profile change needed: RTC slow RAM offsets 0/4 are board-agnostic.

## Version bump
SLEEP_TRACE_VERSION 6 → 7: `_lastShutdownReasonCode` joins the versioned RTC
block (CodeRabbit round-2 finding — an unversioned layout change could let an
older build's RTC data pass validation after an OTA). logSleepBattery() clears
stale-layout data on the mismatch.

## Sleep-trace CSV migration
A pre-marker `sleep_trace.csv` has an 11-column header; rows now carry 12
columns. flushSleepTrace() rotates a legacy file (no `shutdown_reason` in the
header) to `/.crosspoint/sleep_trace_v1.csv` before appending, so schemas never
mix within one file. One generation is kept.
