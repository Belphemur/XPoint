# Design: Auto-detect timezone by IP and apply DST-correct local time

**Status:**
- **Phase 1 (static captured offset):** implemented and **merged** in PR #11 (`8b1c03c2` on `develop`). Verified green (cppcheck + all 4 build envs + host unit tests + clang-format).
- **Phase 2 (automatic DST via timezone library):** **designed here, not yet implemented.** This is a follow-up that supersedes the static-offset storage from Phase 1 — the merged code stores a *momentary* offset that goes stale at the next DST flip; Phase 2 makes DST correct automatically by computing the live offset from the stored IANA id via an Arduino timezone library.
**Date:** 2026-08-27
**Branch (Phase 2):** `feat/tz-dst-automatic` (cut from merged `develop`)
**Decisions (locked, 2026-08-27):** manual-only re-sync · auto-detect on first sync · `setInsecure()` TLS · ipwho.is (primary) + worldtimeapi.org (secondary) · remove the manual UTC-offset picker.

## 1. Goal and summary

Today the clock works like this:

- The RTC stores **UTC** (`syncFromNTP()` calls `configTzTime("UTC0", …)` and writes the
  UTC wall-clock into the DS3231/PCF8563).
- The user picks a **fixed** UTC offset in quarter-hour steps (`clockUtcOffsetQ`, biased by 48;
  range UTC-12:00 … UTC+14:00) in `ClockOffsetActivity`.
- `HalClock::formatTime()` applies that fixed offset to the RTC's UTC every time it renders.

That fixed offset is wrong for half the year in any zone with daylight saving, and it silently
drifts when DST flips. The goal is to **auto-detect the device's IANA timezone from its public IP**
on first sync and then display **DST-correct local time** automatically, re-detecting only when the
user runs a manual sync.

**Decision — remove the manual UTC-offset picker.** `ClockOffsetActivity` and `clockUtcOffsetQ` are
deleted. The device no longer offers a hand-set offset. Auto-detect is the sole source of the display
offset. A device that has never connected to WiFi shows UTC (offset 0) until its first sync detects a
zone. This is the "delete the current settings" instruction: the old offset machinery is gone, replaced
by detected state.

## 2. Research: external services (verified live 2026-08-27)

| Service | URL | Key | HTTPS | IANA id | current offset | is_dst |
|---|---|---|---|---|---|---|
| **ipwho.is** (primary) | `https://ipwho.is/` | none | ✅ | `timezone.id` | `timezone.offset` (s) | `timezone.is_dst` |
| **worldtimeapi.org** (secondary) | `https://worldtimeapi.org/api/ip` | none | ✅ | `timezone` | `utc_offset` (`-04:00`) | `dst` |

Rejected: ip-api.com (HTTP-only without paid key), timeapi.io (returns local time, not an offset),
bigdatacloud (needs API key → 403). The firmware builds with `-DFREEINK_NET_WOLFSSL=1` and ships
`freeink::SecureHttpClient` (wolfSSL TLS 1.3), so the HTTPS call reuses existing infra.

**TLS trust decision:** use `setInsecure()` (consistent with the SDK's existing OTA/OPDS wolfSSL
usage). No root-CA pinning for now.

**ESP32 precedent:** Random Nerd Tutorials (`configTzTime` + POSIX DST rule), Arduino `Timezone` lib
(hardcoded DST rules), and `mmarkin/GeoIP` / r/esp32 ip-api sketches (fetch zone from an IP API). Since
our API returns the *current* offset directly, we skip POSIX-rule complexity: store the offset we're
told and re-detect on manual sync. This also survives governments moving DST dates without a firmware
update.

## 3. Current architecture (what changes where)

- `lib/hal/HalClock.{h,cpp}` — RTC read (UTC), `formatTime(buf,size,utcOffsetQuarterHoursBiased,use12h)`
  applies a **fixed** biased offset; `syncFromNTP()` hardcodes `configTzTime("UTC0", …)`.
- `src/CrossPointSettings.h` — `clockUtcOffsetQ`, `clockFormat`, `clockHasBeenSynced`, `StatusBarSpec`.
- `src/activities/settings/ClockOffsetActivity.{h,cpp}` — **deleted** (manual UTC-offset picker).
- `src/activities/settings/ClockSyncActivity.{h,cpp}` — manual "Sync clock now".
- `src/activities/network/WifiSelectionActivity.cpp:520-528` — auto-sync hook on first WiFi connect.
- `src/activities/settings/StatusBarSettingsActivity.cpp` — clock settings rows (ITEM_CLOCK_UTC_OFFSET →
  ClockOffsetActivity; ITEM_CLOCK_SYNC → ClockSyncActivity).
- `src/SettingsList.h` — web UI `SettingInfo::Value` for `clockUtcOffsetQ`.
- Renderer reads `SETTINGS.clockUtcOffsetQ` via `StatusBarSpec`.

The RTC-stores-UTC invariant stays. We only change **how the display offset is chosen and kept fresh**.

## 4. Proposed data model

Append to `CrossPointSettings` (the old `clockUtcOffsetQ` is removed; see §7):

```cpp
// Auto-detected IANA timezone id, e.g. "America/Toronto". Empty = not detected (show UTC).
char clockTimeZoneId[40] = "";
// Detected current UTC offset in MINUTES (signed), e.g. -240 = UTC-4. 0 = not detected.
int16_t clockTzOffsetMin = 0;
// True when the detected zone is currently in DST (informational; the offset already folds DST).
uint8_t clockTzIsDst = 0;
```

- `int16_t` minutes covers UTC-12:00 (−720) … UTC+14:00 (+840).
- Settings JSON: these are new keys; `fromJson()` ignores unknown keys, so old saves load fine and the
  device falls back to UTC (offset 0, empty id) until first auto-detect.
- **Remove** `clockUtcOffsetQ` from the struct and from `StatusBarSpec`.

### Display resolution (single source of truth)

```cpp
// Effective signed UTC offset in MINUTES for display. UTC (0) until detected.
int clockEffectiveOffsetMin() const {
  return (clockTimeZoneId[0] != '\0' && clockTzOffsetMin != 0) ? clockTzOffsetMin : 0;
}
```

`HalClock::formatTime()` is changed to take **minutes** (not the biased quarter-hour value):

```cpp
// offsetMinutes: signed UTC offset in minutes (e.g. -240 = UTC-4, +330 = UTC+5:30).
bool formatTime(char* buf, size_t bufSize, int offsetMinutes, bool use12Hour) const;
```

Update the two remaining callers (`ClockSyncActivity`, status bar) to pass
`SETTINGS.clockEffectiveOffsetMin()`.

## 5. Timezone detection module

New file `lib/hal/HalTimeZone.{h,cpp}` — small, dependency-light (no app settings, no UI):

```cpp
namespace freeink {
struct TimeZoneInfo {
  bool valid = false;
  char id[40] = "";      // IANA id
  int offsetMin = 0;      // current UTC offset in minutes (folds DST)
  bool isDst = false;
};

// Resolve the caller's timezone from their public IP. Requires WiFi.
// Tries primary (ipwho.is) then secondary (worldtimeapi.org).
// Returns valid=false on no-WiFi / all endpoints failed / parse error.
TimeZoneInfo detectTimeZoneFromIp();
}
```

Implementation notes:

- Use `freeink::SecureHttpClient` (wolfSSL) for HTTPS; call `setInsecure()` (decision §2).
- Parse with `ArduinoJson` (dep 7.4.2). ipwho.is: `doc["timezone"]["id"]`,
  `doc["timezone"]["offset"]` (seconds → /60), `doc["timezone"]["is_dst"]`. worldtimeapi:
  `doc["timezone"]`, `doc["utc_offset"]` (`"-04:00"` → parse sign/hh/mm), `doc["dst"]`.
- Timeout ~10 s; no aggressive retry (background nicety, never block the clock).
- On success copy `id`/`offsetMin`/`isDst`; on primary failure try secondary; if both fail
  `valid = false` and the caller keeps its current (UTC) state.

## 6. Wiring it into sync

**Decision — re-detect only on manual "Sync clock now" (not on every WiFi connect); auto-detect on
first sync.** No daily-debounce: the offset is set once on first sync and stays until the user
manually re-syncs.

### Auto-sync hook (`WifiSelectionActivity.cpp:520-528`) — first sync only

```cpp
if (halClock.isAvailable() && !SETTINGS.clockHasBeenSynced) {
  if (halClock.syncFromNTP()) {
    SETTINGS.clockHasBeenSynced = 1;
    auto tz = freeink::detectTimeZoneFromIp();
    if (tz.valid) {
      SETTINGS.clockTzOffsetMin = tz.offsetMin;
      SETTINGS.clockTzIsDst = tz.isDst ? 1 : 0;
      strncpy(SETTINGS.clockTimeZoneId, tz.id, sizeof(SETTINGS.clockTimeZoneId) - 1);
    }
    SETTINGS.saveToFile();
  }
}
```

### Manual sync (`ClockSyncActivity::runSync`) — re-detect

After `syncFromNTP()` succeeds, also call `detectTimeZoneFromIp()` and store it (same block as above).
"Sync clock now" becomes "sync time **and** re-detect timezone" — exactly what you want after traveling.
`clockHasBeenSynced` stays set; manual sync is always allowed.

## 7. Settings UI changes (remove manual picker)

- **Delete** `ClockOffsetActivity.{h,cpp}` and the `clockUtcOffsetQ` field.
- The clock section in `StatusBarSettingsActivity` shows, instead of the offset picker:
  - Detected IANA id + current offset + DST badge (e.g. `America/Toronto · UTC-4 · EDT`), or
    `UTC` / `STR_NOT_SET` before first detection.
  - The existing "Sync clock now" row (`ITEM_CLOCK_SYNC` → `ClockSyncActivity`) stays and now also
    re-detects the zone.
- New string ids: `STR_TIMEZONE`, `STR_TIMEZONE_DETECTING`, `STR_TIMEZONE_DETECT_FAILED`.
- Web UI (`SettingsList.h`): remove the `clockUtcOffsetQ` `SettingInfo::Value`; optionally show
  `clockTimeZoneId` as read-only text and keep `clockFormat`.

## 8. Edge cases / robustness

- **No WiFi at sync time:** detection `valid=false` → device stays at UTC (offset 0); clock still
  works. No crash, no hang.
- **Detection API down / rate-limited:** same — UTC fallback, `TZ` warnings logged.
- **VPN / cellular / wrong geo:** IP geolocation is approximate; the UI shows the detected id so the
  user can see a bad guess. No manual override remains by decision — a future "set manually" can be
  added if users ask.
- **RTC lost power:** unchanged — `getDateTime()` already rejects invalid dates; detection only changes
  the display offset, not the RTC value.
- **Travel across zones while offline:** re-detect on next manual "Sync clock now".

## 9. Privacy

- The device sends its **public IP** to a third-party geolocation service to learn the timezone. No
  account, no key, no PII beyond the IP the service already sees. Call is made only during an explicit
  or first sync, not continuously. Document this in the user guide's privacy section.

## 10. Implementation plan

1. `HalClock::formatTime` → change to minute-based offset signature; update callers.
2. `CrossPointSettings`: remove `clockUtcOffsetQ` (struct + `StatusBarSpec`); add `clockTimeZoneId`,
   `clockTzOffsetMin`, `clockTzIsDst`; add `clockEffectiveOffsetMin()`; migrate `fromJson`/`toJson`.
3. `lib/hal/HalTimeZone.{h,cpp}`: `detectTimeZoneFromIp()` over `SecureHttpClient` + `ArduinoJson`,
   ipwho.is → worldtimeapi.org fallback.
4. Wire detection into `WifiSelectionActivity` auto-sync (first sync only) and `ClockSyncActivity`
   manual sync.
5. UI: delete `ClockOffsetActivity`; update `StatusBarSettingsActivity` clock rows; add string ids;
   update web `SettingsList.h` (remove `clockUtcOffsetQ`).
6. Docs: update `USER_GUIDE.md` clock section + privacy note; mark this doc implemented.

## 11. Open questions — RESOLVED

- **Q1 DST refresh cadence (Phase 1):** manual "Sync clock now" only (no auto re-detect per WiFi
  connect, no daily debounce). ✅ — **superseded by Phase 2** (see §13): DST is now computed live from
  the stored IANA id, so the displayed offset corrects itself at every DST boundary without any resync.
- **Q2 Service pair:** ipwho.is primary, worldtimeapi.org secondary. ✅
- **Q3 New-device default:** auto-detect on first sync. ✅
- **Q4 TLS trust:** `setInsecure()` (no CA pin). ✅
- **Q5 UI label / manual picker:** manual UTC-offset picker removed; show detected `America/Toronto
  (UTC-4)` form on the status-bar clock row. ✅

## 12. Testing

- Unit: `HalTimeZone` parse against captured ipwho.is / worldtimeapi JSON fixtures (offline).
- Unit: `clockEffectiveOffsetMin()` (UTC before detect, minutes after).
- Unit: `formatTime` minute-overload wrapping across midnight and the UTC±date line.
- Manual on-device: connect WiFi → RTC UTC unchanged, status bar shows local DST-correct time; run
  "Sync clock now" after a zone change → offset updates; kill WiFi at sync → UTC fallback, no hang.
- CI: `pio check -e x4pro` (cppcheck) must stay green; `clang-format` clean.

## 13. Phase 2 — automatic DST via a timezone library (follow-up, supersedes static offset)

### 13.1 Why Phase 1 is insufficient

Phase 1 stores `clockTzOffsetMin` = the offset **at the moment of detection** (e.g. `-240`
for `America/Toronto` in summer). `HalClock::formatTime()` adds that fixed number to the RTC's
UTC every time it renders. That is correct only until the next DST transition, after which the
display is one hour wrong until the user manually re-syncs. The offset capture is a *snapshot*;
it cannot know when DST flips.

### 13.2 The right model

Store the **IANA id** as the single source of truth (already captured by `detectTimeZoneFromIp()`
into `clockTimeZoneId`). At display time, resolve that id to a **live** UTC offset using a timezone
library that knows the IANA rule database — so the offset is recomputed for the *current* UTC
instant every render. When the wall clock crosses a DST boundary, the computed offset changes by
±1h automatically. **No extra network call, no resync, no DB we maintain.**

`clockTzOffsetMin` / `clockTzIsDst` become *informational caches* (used for the status-bar badge
before the library is first queried, and as a UTC fallback if the id is unknown to the DB); the
authoritative display offset is always `libraryOffset(utcNow, clockTimeZoneId)`.

### 13.3 Library evaluation

| Option | Method | Live DST from IANA id? | Cost on this device (16 MB flash) |
|---|---|---|---|
| **AceTime** (`bxparks/AceTime`) | Bundled IANA rule DB (`zonedb`/`zonedbx`/`zonedbx2025`), `forZoneName()` → `ZoneProcessor::getOffsetInfo(epoch)` | ✅ full, no network | `BasicZoneProcessor`+`zonedb` ≈35 kB flash; `ExtendedZoneProcessor`+`zonedbx` ≈44 kB; `…2025` recent-years variant (tz 2025b, 2025–2200) smaller. RAM: ~hundreds of bytes per ZoneProcessor (flash-resident const data). Negligible vs 16 MB. **Chosen: `ExtendedZoneProcessor`+`zonedbx2025`.** |
| **ezTime** (`ropg/ezTime`) | Wraps newlib `tzset()`/POSIX TZ | ❌ ESP32 newlib ships **no zoneinfo**, so an IANA id cannot resolve to future DST. Only works with a POSIX TZ string (which we'd have to derive). | tiny, but insufficient. |
| Native `setenv("TZ", posixRule); tzset()` | C library | ❌ requires translating IANA→POSIX rule = a fragile data problem of its own | zero, but blocked on the translation. |

**Recommended: `bxparks/AceTime`, `ExtendedZoneProcessor` + `zonedbx` (or `zonedbx2025` to trim flash).**
Rationale: it is the only option that satisfies the requirement (live DST from a stored IANA id)
with no network and no hand-maintained DB; the flash cost (~35–44 kB) is trivial on a 16 MB part;
and it is the de-facto standard ESP32/Arduino TZ library. `Extended` (not `Basic`) is chosen so
irregular DST zones (e.g. `America/Sao_Paulo`, historical changes) are correct; if flash/IRAM
proves tight on the C3 `default` env we fall back to `Basic`+`zonedb` which still covers the vast
majority of users.

### 13.4 Proposed changes (Phase 2)

- **Dependency:** add `bxparks/AceTime` to `platformio.ini` `lib_deps` (same line style as the
  existing `bblanchon/ArduinoJson @ 7.4.2`).
- **`CrossPointSettings`:**
  - Keep `clockTimeZoneId[40]` (now the source of truth).
  - Demote `clockTzOffsetMin`/`clockTzIsDst` to caches (still persisted for the badge + offline
    fallback; no behavior change to the JSON schema).
  - `clockEffectiveOffsetMin()` becomes: if `clockTimeZoneId` is set, ask the HAL to resolve it
    (`HalClock::offsetMinutesFor(utcNow, id)`); else return the cached `clockTzOffsetMin` (UTC 0
    before first detect).
- **`HalClock`:** add a single `ace_time::ExtendedZoneProcessor` (+ manager or direct
  `forZoneName`) cached by id, exposed as `int offsetMinutesFor(time_t utcEpoch, const char* ianaId)`
  returning the DST-aware signed offset in minutes (fallback: `clockTzOffsetMin`/0 on unknown id).
  `formatTime()` keeps its minute-offset signature — callers pass the *resolved* offset.
- **Wiring:** none required beyond `clockEffectiveOffsetMin()` — every existing caller
  (`StatusBarSettingsActivity`, `ClockSyncActivity`, `ReadingStatsUtils`) already goes through it,
  so DST becomes automatic everywhere. `syncFromNTP()` keeps `configTzTime("UTC0", …)` (RTC stays UTC).
- **Sync activities** (`WifiSelectionActivity` first-sync, `ClockSyncActivity` manual): still store
  `clockTimeZoneId` from detection; the offset/badge cache is refreshed from the library right after
  detection for immediate display correctness, but the *runtime* offset is always recomputed live.

### 13.5 Verification

- Unit: given a fixed UTC epoch **before** and **after** a known DST boundary for a zone (e.g.
  `America/Toronto` 2026-03-08 06:59 UTC → −300, 07:00 UTC → −240), assert the resolved offset
  flips by ±60 min. Same for a southern-hemisphere zone.
- Unit: unknown id + cached fallback path returns the stored `clockTzOffsetMin`.
- Host unit tests: extend the existing `clockEffectiveOffsetMin()` test to mock the HAL resolver
  (the host shim already stubs `CrossPointSettings`); no real AceTime needed in host tests.
- On-device: set `America/New_York`, confirm the status bar shows EST in winter / EDT in summer with
  no resync; fast-forward the RTC across a boundary → display corrects.
- CI: `pio check` (cppcheck) + clang-format + all 4 build envs + host unit tests remain green
  (expect a small flash-size bump from the AceTime DB — well within 16 MB).

### 13.6 Open decision (needs your sign-off before code)

- **AceTime `Extended`+`zonedbx2025` (recommended, full coverage) vs `Basic`+`zonedb` (smaller,
  covers ~95% of zones)?** → recommending Extended unless the C3 `default` build shows flash pressure.
- Add as a **new PR on the fork** (`feat/tz-dst-automatic` → `develop`), separate from the merged
  static-offset PR #11, per the "one PR per feature / design-doc-first" gate.
