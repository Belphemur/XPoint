#include "HalTimeZone.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace {
constexpr const char* TAG = "HalTimeZone";
constexpr int kHttpTimeoutMs = 10000;
// Supported UTC offset range (mirrors HalClock::formatTime). Out-of-range
// values can only come from a malformed upstream response, so we clamp here
// rather than trusting every caller to defend against outliers.
constexpr int kMinOffsetMin = -720;  // UTC-12
constexpr int kMaxOffsetMin = 840;   // UTC+14
int clampOffsetMin(int minutes) {
  if (minutes < kMinOffsetMin) return kMinOffsetMin;
  if (minutes > kMaxOffsetMin) return kMaxOffsetMin;
  return minutes;
}

bool parseIpWhoIs(const std::string& body, freeink::TimeZoneInfo& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    return false;
  }
  if (!doc["success"].as<bool>()) {
    return false;
  }
  const char* id = doc["timezone"]["id"].as<const char*>();
  if (!id || !*id) {
    return false;
  }
  strncpy(out.id, id, sizeof(out.id) - 1);
  out.id[sizeof(out.id) - 1] = '\0';
  out.offsetMin = doc["timezone"]["offset"].as<int>() / 60;
  out.isDst = doc["timezone"]["is_dst"].as<bool>();
  out.valid = true;
  return true;
}

// Parses "[+/-]HH:MM" into signed minutes.
bool parseUtcOffset(const char* s, int& offsetMin) {
  if (!s) {
    return false;
  }
  int sign = 1;
  if (*s == '+') {
    ++s;
  } else if (*s == '-') {
    sign = -1;
    ++s;
  }
  if (strlen(s) != 5 || s[2] != ':') {
    return false;
  }
  for (int i : {0, 1, 3, 4}) {
    if (!isdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  const int hours = (s[0] - '0') * 10 + (s[1] - '0');
  const int minutes = (s[3] - '0') * 10 + (s[4] - '0');
  offsetMin = sign * (hours * 60 + minutes);
  return true;
}

bool parseWorldTimeApi(const std::string& body, freeink::TimeZoneInfo& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    return false;
  }
  const char* id = doc["timezone"].as<const char*>();
  if (!id || !*id) {
    return false;
  }
  int offsetMin = 0;
  if (!parseUtcOffset(doc["utc_offset"].as<const char*>(), offsetMin)) {
    return false;
  }
  strncpy(out.id, id, sizeof(out.id) - 1);
  out.id[sizeof(out.id) - 1] = '\0';
  out.offsetMin = offsetMin;
  out.isDst = doc["dst"].as<bool>();
  out.valid = true;
  return true;
}

struct TzService {
  const char* url;
  bool (*parse)(const std::string&, freeink::TimeZoneInfo&);
};

constexpr TzService kServices[] = {
    {"https://ipwho.is/", &parseIpWhoIs},
    {"https://worldtimeapi.org/api/ip", &parseWorldTimeApi},
};
}  // namespace

namespace freeink {

TimeZoneInfo detectTimeZoneFromIp() {
  TimeZoneInfo out;
  if (WiFi.status() != WL_CONNECTED) {
    LOG_DBG(TAG, "No WiFi connection");
    return out;
  }

  for (const auto& svc : kServices) {
    SecureHttpClient http;
    http.setInsecure();
    http.setTimeout(kHttpTimeoutMs);
    if (!http.begin(svc.url)) {
      LOG_DBG(TAG, "begin() failed: %s", svc.url);
      continue;
    }
    const int code = http.GET();
    if (code < 200 || code >= 300) {
      LOG_DBG(TAG, "%s -> HTTP %d", svc.url, code);
      http.end();
      continue;
    }
    const std::string body = http.getString();
    http.end();
    if (svc.parse(body, out)) {
      out.offsetMin = clampOffsetMin(out.offsetMin);
      LOG_INF(TAG, "Zone via %s: %s (UTC%+d min%s)", svc.url, out.id, out.offsetMin, out.isDst ? ", DST" : "");
      return out;
    }
    LOG_DBG(TAG, "Unparseable response: %s", svc.url);
  }

  LOG_ERR(TAG, "All time zone services failed");
  return out;
}

}  // namespace freeink

#include <AceTime.h>

namespace freeink {

using ace_time::ExtendedZoneManager;
using ace_time::ExtendedZoneProcessor;
using ace_time::ExtendedZoneProcessorCache;
using ace_time::TimeZone;
using ace_time::ZonedDateTime;
using ace_time::ZonedExtra;
using ace_time::zonedbx2025::kZoneAndLinkRegistry;
using ace_time::zonedbx2025::kZoneAndLinkRegistrySize;

// One zone processor is enough: we resolve at most one zone at a time, and the
// manager reuses the processor across calls. The database (kZoneAndLinkRegistry,
// 597 entries incl. Links like "America/Toronto") is the 2025b TZ Database
// covering 2025-2200, flash-resident const data.
static ExtendedZoneProcessorCache<1> sZoneCache;
static ExtendedZoneManager sZoneManager(kZoneAndLinkRegistrySize, kZoneAndLinkRegistry, sZoneCache);

// Serialize the resolver + cache: it can be entered from both the main task and
// a render task, and the cache/mutable manager state must not race.
static std::mutex sResolverMutex;

// Cache the last successful resolution so callers that render every frame (menu
// rows) don't rebuild the ZonedDateTime repeatedly for the same zone+minute.
static char sCachedId[40] = "";
static int64_t sCachedEpoch = -1;
static int sCachedOffset = 0;

// 2026-01-01T00:00:00Z. Anything earlier means the system clock was never
// seeded (HalClock::seedSystemClockFromRtc) and never synced (SNTP), so the
// caller must fall back to its cached offset.
// Deliberately 32-bit and unsigned: Unix time stays below 2^32 until 2106, so
// the gate needs no 64-bit constant, and comparing it against the int64_t
// parameter is a widening conversion (no signed/unsigned mix). Only the AceTime
// call below genuinely needs 64 bits.
static constexpr uint32_t MIN_PLAUSIBLE_EPOCH_SECONDS = 1767225600;

namespace {

// Reject an empty id and any epoch outside the firmware's supported window (see
// MIN_PLAUSIBLE_EPOCH_SECONDS). Shared so the resolver's cache can never serve
// an offset computed from a nonsensical epoch.
bool isValidZoneRequest(const char* ianaId, int64_t utcEpochSeconds) {
  return ianaId != nullptr && ianaId[0] != '\0' && utcEpochSeconds >= MIN_PLAUSIBLE_EPOCH_SECONDS;
}

// Front half of both resolvers. Callers must hold sResolverMutex: the zone
// manager keeps mutable processor state behind it, so the returned
// ZonedDateTime is only valid while the lock is held.
bool resolveZonedDateTime(const char* ianaId, int64_t utcEpochSeconds, ZonedDateTime& out) {
  if (!isValidZoneRequest(ianaId, utcEpochSeconds)) return false;
  const TimeZone tz = sZoneManager.createForZoneName(ianaId);
  if (tz.isError()) return false;
  // utcEpochSeconds is Unix time (seconds since 1970). AceTime's internal epoch
  // is 2000-01-01, so use forUnixSeconds64() which converts for us; passing raw
  // Unix seconds to forEpochSeconds() would be off by 30 years.
  out = ZonedDateTime::forUnixSeconds64(utcEpochSeconds, tz);
  return true;
}

}  // namespace

bool resolveUtcOffsetMinutes(const char* ianaId, int64_t utcEpochSeconds, int& offsetMin) {
  std::lock_guard<std::mutex> lock(sResolverMutex);
  // Serve from cache when the id and wall-minute are unchanged.
  if (isValidZoneRequest(ianaId, utcEpochSeconds) && sCachedId[0] != '\0' &&
      strncmp(sCachedId, ianaId, sizeof(sCachedId)) == 0 && utcEpochSeconds / 60 == sCachedEpoch / 60) {
    offsetMin = sCachedOffset;
    return true;
  }

  ZonedDateTime zdt;
  if (!resolveZonedDateTime(ianaId, utcEpochSeconds, zdt)) return false;
  const int off = zdt.timeOffset().toMinutes();

  strncpy(sCachedId, ianaId, sizeof(sCachedId) - 1);
  sCachedId[sizeof(sCachedId) - 1] = '\0';
  sCachedEpoch = utcEpochSeconds;
  sCachedOffset = off;
  offsetMin = off;
  return true;
}

bool resolveZoneAbbreviation(const char* ianaId, int64_t utcEpochSeconds, char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) return false;
  std::lock_guard<std::mutex> lock(sResolverMutex);
  ZonedDateTime zdt;
  if (!resolveZonedDateTime(ianaId, utcEpochSeconds, zdt)) return false;
  // ZonedExtra takes AceTime-epoch seconds (2000-based), so convert through the
  // ZonedDateTime we already resolved rather than passing Unix seconds in.
  const ZonedExtra extra = ZonedExtra::forEpochSeconds(zdt.toEpochSeconds(), zdt.timeZone());
  if (extra.isError()) return false;
  const char* abbrev = extra.abbrev();
  if (abbrev[0] == '\0') return false;
  strncpy(out, abbrev, outSize - 1);
  out[outSize - 1] = '\0';
  return true;
}

}  // namespace freeink
