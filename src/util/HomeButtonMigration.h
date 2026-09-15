#pragma once

#include <cstdint>

#include "util/HomeButtonInput.h"

// One-time migration policy for the fork's pre-unification home catalogs.
// Kept pure so host tests can pin the persisted maps and old-key precedence.
namespace home_button_migration {

// Persisted by the fork's pre-unification HOME_ACT_* catalog.
inline constexpr HomeButtonAction LEGACY_HOME_ACTIONS[] = {
    HomeButtonAction::Ignore, HomeButtonAction::ToggleFrontlight, HomeButtonAction::Home,  HomeButtonAction::ReaderMenu,
    HomeButtonAction::Sleep,  HomeButtonAction::Screenshot,       HomeButtonAction::GoBack};

// Persisted by the fork's legacy Confirm-hold LP_MENU_* catalog.
inline constexpr HomeButtonAction LEGACY_HOLD_ACTIONS[] = {HomeButtonAction::Sync, HomeButtonAction::Ignore,
                                                           HomeButtonAction::Bookmark, HomeButtonAction::Dictionary,
                                                           HomeButtonAction::ReaderMenu};

constexpr HomeButtonAction migrateLegacyHomeAction(const uint8_t value) {
  return value < sizeof(LEGACY_HOME_ACTIONS) / sizeof(LEGACY_HOME_ACTIONS[0]) ? LEGACY_HOME_ACTIONS[value]
                                                                              : HomeButtonAction::Ignore;
}

constexpr HomeButtonAction migrateLegacyHoldAction(const uint8_t value) {
  return value < sizeof(LEGACY_HOLD_ACTIONS) / sizeof(LEGACY_HOLD_ACTIONS[0]) ? LEGACY_HOLD_ACTIONS[value]
                                                                              : HomeButtonAction::Ignore;
}

// The old double-click key marks a whole legacy home-field group and takes
// precedence over the older Confirm-hold key; the latter only applies when the
// group is absent and the board exposed that setting.
enum class LegacySource : uint8_t { None, HomeCatalog, HoldCatalog };

constexpr LegacySource legacySource(const bool hasLegacyHomeKey, const bool hasLegacyHoldKey,
                                    const bool menuButtonCapable) {
  if (hasLegacyHomeKey) return LegacySource::HomeCatalog;
  return menuButtonCapable && hasLegacyHoldKey ? LegacySource::HoldCatalog : LegacySource::None;
}

// Sparse legacy files may omit a field entirely: absent means the new
// initializer default, not the legacy catalog's index 0. Only serialized home
// fields participate in the one-time value remap.
struct MigratedAction {
  bool migrated;
  HomeButtonAction action;
};

constexpr MigratedAction migrateLegacyHomeField(const bool present, const uint8_t value) {
  if (!present) return {false, static_cast<HomeButtonAction>(value)};
  if (value >= sizeof(LEGACY_HOME_ACTIONS) / sizeof(LEGACY_HOME_ACTIONS[0])) {
    return {false, static_cast<HomeButtonAction>(value)};
  }
  return {true, LEGACY_HOME_ACTIONS[value]};
}

constexpr MigratedAction migrateLegacyHoldField(const bool present, const uint8_t value) {
  if (!present) return {false, static_cast<HomeButtonAction>(value)};
  if (value >= sizeof(LEGACY_HOLD_ACTIONS) / sizeof(LEGACY_HOLD_ACTIONS[0])) {
    return {false, static_cast<HomeButtonAction>(value)};
  }
  return {true, LEGACY_HOLD_ACTIONS[value]};
}

}  // namespace home_button_migration
