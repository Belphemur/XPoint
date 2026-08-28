#include "HalTimeZone.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <cctype>
#include <cstring>
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
using ace_time::zonedbx2025::kZoneAndLinkRegistry;
using ace_time::zonedbx2025::kZoneAndLinkRegistrySize;

// One zone processor is enough: we resolve at most one zone at a time, and the
// manager reuses the processor across calls. The database (kZoneAndLinkRegistry,
// 597 entries incl. Links like "America/Toronto") is the 2025b TZ Database
// covering 2025-2200, flash-resident const data.
static ExtendedZoneProcessorCache<1> sZoneCache;
static ExtendedZoneManager sZoneManager(kZoneAndLinkRegistrySize, kZoneAndLinkRegistry, sZoneCache);

// Cache the last successful resolution so callers that render every frame (menu
// rows) don't rebuild the ZonedDateTime repeatedly for the same zone+minute.
static char sCachedId[40] = "";
static int64_t sCachedEpoch = -1;
static int sCachedOffset = 0;

bool resolveUtcOffsetMinutes(const char* ianaId, int64_t utcEpochSeconds, int& offsetMin) {
  if (!ianaId || ianaId[0] == '\0') {
    return false;
  }
  // Serve from cache when the id and wall-minute are unchanged.
  if (sCachedId[0] != '\0' && strncmp(sCachedId, ianaId, sizeof(sCachedId)) == 0 &&
      utcEpochSeconds / 60 == sCachedEpoch / 60) {
    offsetMin = sCachedOffset;
    return true;
  }

  const TimeZone tz = sZoneManager.createForZoneName(ianaId);
  if (tz.isError()) {
    return false;
  }
  const ZonedDateTime zdt = ZonedDateTime::forEpochSeconds(static_cast<ace_time::acetime_t>(utcEpochSeconds), tz);
  const int off = zdt.timeOffset().toMinutes();

  strncpy(sCachedId, ianaId, sizeof(sCachedId) - 1);
  sCachedId[sizeof(sCachedId) - 1] = '\0';
  sCachedEpoch = utcEpochSeconds;
  sCachedOffset = off;
  offsetMin = off;
  return true;
}

}  // namespace freeink
