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
      LOG_INF(TAG, "Zone via %s: %s (UTC%+d min%s)", svc.url, out.id, out.offsetMin, out.isDst ? ", DST" : "");
      return out;
    }
    LOG_DBG(TAG, "Unparseable response: %s", svc.url);
  }

  LOG_ERR(TAG, "All time zone services failed");
  return out;
}

}  // namespace freeink
