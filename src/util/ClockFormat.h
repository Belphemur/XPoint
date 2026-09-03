#pragma once

#include <cstddef>
#include <ctime>

namespace ClockFormat {

// Longest rendering of formatTimestamp(), terminator included:
// "2026-09-03 12:47:42 PM -04:00" (12h form with the offset fallback).
constexpr size_t kTimestampSize = 32;

// Formats a UTC epoch as local wall-clock time:
//   24h: "2026-09-03 00:47:42 EDT"
//   12h: "2026-09-03 12:47:42 AM EDT"
//
// ZZZ is the zone abbreviation resolved from `zoneId` (EST/EDT/UTC/...). When
// the zone is unknown or unresolved the abbreviation is replaced by a
// "+HH:MM"/"-HH:MM" offset, so the string is never ambiguous.
//
// Seconds come from the epoch rather than the RTC: HalClock only exposes
// minute resolution, and callers here already have a system clock that has
// been seeded (HalClock::seedSystemClockFromRtc) or synced (SNTP).
//
// Returns false when the buffer is too small (nothing is written) or the epoch
// is not a usable time; `out` is left empty in that case.
bool formatTimestamp(char* out, size_t outSize, time_t utcEpoch, int offsetMinutes, const char* zoneId,
                     bool use12Hour = false);

}  // namespace ClockFormat
