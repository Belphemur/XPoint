#pragma once

#include <I18n.h>

#include "CrossPointSettings.h"

namespace power_menu {
// Rows of the device-side Power submenu, in display order. The settings
// themselves stay in the shared SettingsList (so the web settings API keeps
// working); this only declares which of them the submenu owns, so
// SettingsActivity can hide the flat duplicates.
//
// Idle Clock and Page Render Clock have no backing setting: they report the rung
// the endurance governor resolved at runtime (profile base plus any crash-strike
// promotion), which is the only way to see what the ladder actually did.
inline constexpr StrId ROW_LABELS[] = {StrId::STR_POWER_PROFILE,     StrId::STR_IDLE_CLOCK,
                                       StrId::STR_PAGE_RENDER_CLOCK, StrId::STR_HEAVY_JOB_BOOST,
                                       StrId::STR_POWER_STATS,       StrId::STR_TIME_TO_SLEEP,
                                       StrId::STR_AUTO_POWER_OFF};
inline constexpr int ROW_COUNT = static_cast<int>(sizeof(ROW_LABELS) / sizeof(ROW_LABELS[0]));

// Value-pointer identity for the settings the submenu absorbs. Used to filter
// them out of the flat category lists.
inline constexpr uint8_t CrossPointSettings::* FIELDS[] = {&CrossPointSettings::powerProfile,
                                                           &CrossPointSettings::powerStatsMode,
                                                           &CrossPointSettings::powerHeavyJobBoost,
                                                           &CrossPointSettings::sleepTimeoutMinutes,
                                                           &CrossPointSettings::autoPowerOffHours};

inline bool isSetting(uint8_t CrossPointSettings::* field) {
  for (auto candidate : FIELDS) {
    if (candidate == field) return true;
  }
  return false;
}
}  // namespace power_menu
