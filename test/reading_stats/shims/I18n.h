#pragma once

// Host test shim for I18n.h — supplies just enough for BookReadingStats.cpp
// (which uses tr(STR_STATS_LESS_THAN_MIN) in formatDuration).

enum class StrId { STR_STATS_LESS_THAN_MIN = 0 };

class I18n {
 public:
  static I18n& getInstance() {
    static I18n instance;
    return instance;
  }
  const char* get(StrId) const { return "< 1 min"; }
};

#define tr(id) I18n::getInstance().get(StrId::id)
