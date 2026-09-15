#include <gtest/gtest.h>

#include <cstdint>

#include "util/HomeButtonInput.h"
#include "util/HomeButtonMigration.h"

using A = HomeButtonAction;
using home_button_migration::migrateLegacyHoldAction;
using home_button_migration::migrateLegacyHomeAction;
using LegacySource = home_button_migration::LegacySource;

TEST(HomeButtonValues, UpstreamAndForkIndicesAreStable) {
  EXPECT_EQ(static_cast<uint8_t>(A::Home), 0);
  EXPECT_EQ(static_cast<uint8_t>(A::Ignore), 1);
  EXPECT_EQ(static_cast<uint8_t>(A::NextPage), 2);
  EXPECT_EQ(static_cast<uint8_t>(A::Refresh), 3);
  EXPECT_EQ(static_cast<uint8_t>(A::Footnotes), 4);
  EXPECT_EQ(static_cast<uint8_t>(A::Confirm), 5);
  EXPECT_EQ(static_cast<uint8_t>(A::Sync), 6);
  EXPECT_EQ(static_cast<uint8_t>(A::Bookmark), 7);
  EXPECT_EQ(static_cast<uint8_t>(A::Dictionary), 8);
  EXPECT_EQ(static_cast<uint8_t>(A::ReaderMenu), 9);
  EXPECT_EQ(static_cast<uint8_t>(A::ToggleFrontlight), 10);
  EXPECT_EQ(static_cast<uint8_t>(A::Sleep), 11);
  EXPECT_EQ(static_cast<uint8_t>(A::Screenshot), 12);
  EXPECT_EQ(static_cast<uint8_t>(A::GoBack), 13);
  EXPECT_EQ(static_cast<uint8_t>(A::Count), 14);
}

TEST(HomeButtonMigration, MapsLegacyHomeCatalog) {
  EXPECT_EQ(migrateLegacyHomeAction(0), A::Ignore);
  EXPECT_EQ(migrateLegacyHomeAction(1), A::ToggleFrontlight);
  EXPECT_EQ(migrateLegacyHomeAction(2), A::Home);
  EXPECT_EQ(migrateLegacyHomeAction(3), A::ReaderMenu);
  EXPECT_EQ(migrateLegacyHomeAction(4), A::Sleep);
  EXPECT_EQ(migrateLegacyHomeAction(5), A::Screenshot);
  EXPECT_EQ(migrateLegacyHomeAction(6), A::GoBack);
}

TEST(HomeButtonMigration, MapsLegacyHoldCatalog) {
  EXPECT_EQ(migrateLegacyHoldAction(0), A::Sync);
  EXPECT_EQ(migrateLegacyHoldAction(1), A::Ignore);
  EXPECT_EQ(migrateLegacyHoldAction(2), A::Bookmark);
  EXPECT_EQ(migrateLegacyHoldAction(3), A::Dictionary);
  EXPECT_EQ(migrateLegacyHoldAction(4), A::ReaderMenu);
}

TEST(HomeButtonMigration, AlreadyMigratedValuesAreNotRemapped) {
  for (uint8_t value = static_cast<uint8_t>(A::Bookmark); value < static_cast<uint8_t>(A::Count); ++value) {
    EXPECT_EQ(migrateLegacyHomeAction(value), A::Ignore);
    EXPECT_EQ(migrateLegacyHoldAction(value), A::Ignore);
  }
}

TEST(HomeButtonMigration, LegacyHomeKeyTakesPrecedence) {
  EXPECT_EQ(home_button_migration::legacySource(true, true, true), LegacySource::HomeCatalog);
  EXPECT_EQ(home_button_migration::legacySource(true, true, false), LegacySource::HomeCatalog);
}

TEST(HomeButtonMigration, LegacyHoldKeyIsMenuBoardOnlyFallback) {
  EXPECT_EQ(home_button_migration::legacySource(false, true, true), LegacySource::HoldCatalog);
  EXPECT_EQ(home_button_migration::legacySource(false, true, false), LegacySource::None);
  EXPECT_EQ(home_button_migration::legacySource(false, false, true), LegacySource::None);
  EXPECT_EQ(home_button_migration::legacySource(false, false, false), LegacySource::None);
}
