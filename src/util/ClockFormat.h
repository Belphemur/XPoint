#pragma once

#include <cstddef>
#include <ctime>

namespace ClockFormat {

// Longest rendering of formatDateAndTime(), terminator included:
// "2026-09-03 12:47 PM".
constexpr size_t kDateAndTimeSize = 24;

// Longest rendering of formatTimestamp(), terminator included:
// "2026-09-03 12:47:42 PM -04:00".
constexpr size_t kTimestampSize = 32;

// Formats a UTC epoch as local wall-clock date and time, no seconds:
//   24h: "2026-09-03 00:47"
//   12h: "2026-09-03 12:47 AM"
//
// Returns false when the buffer is too small (nothing is written) or the epoch
// is not a usable time; `out` is left empty in that case.
bool formatDateAndTime(char* out, size_t outSize, time_t utcEpoch, int offsetMinutes, bool use12Hour = false);

// As formatDateAndTime(), plus seconds and the zone in effect:
//   24h: "2026-09-03 00:47:42 EDT"
//   12h: "2026-09-03 12:47:42 AM EDT"
//
// ZZZ is the zone abbreviation resolved from `zoneId` (EST/EDT/UTC/...). When
// the zone is unknown or unresolved the abbreviation is replaced by a
// "+HH:MM"/"-HH:MM" offset, so the string is never ambiguous.
bool formatTimestamp(char* out, size_t outSize, time_t utcEpoch, int offsetMinutes, const char* zoneId,
                     bool use12Hour = false);

// Seconds come from the epoch rather than the RTC: HalClock only exposes minute
// resolution, and callers here already have a system clock that has been
// seeded (HalClock::seedSystemClockFromRtc) or synced (SNTP).

}  // namespace ClockFormat
