// Regression tests for the ActivityManager pop-result guard
// (normalizeActivityResult). PR #75 flipped every empty-data result to
// cancelled, which turned ConfirmationActivity's explicit Confirm
// (isCancelled=false, data=monostate) into a Cancel and made the SD-card
// firmware update flow impossible to start. The policy is: only a result from
// an activity that never called setResult() is normalized to cancelled.
#include <gtest/gtest.h>

#include <variant>

#include "activities/ActivityResult.h"

namespace {

// Activity::setResult() (src/activities/Activity.cpp) is the only way an
// activity delivers a result to ActivityManager; it stamps hasResult=true.
ActivityResult asDeliveredViaSetResult(ActivityResult r) {
  r.hasResult = true;
  return r;
}

}  // namespace

// PR #75's original protection must survive: a default-constructed result
// (activity popped without calling setResult()) is normalized to cancelled.
TEST(NormalizeActivityResult, DefaultConstructedResultIsNormalizedToCancelled) {
  ActivityResult r;
  EXPECT_TRUE(normalizeActivityResult(r));
  EXPECT_TRUE(r.isCancelled);
}

// The regression: an explicit Confirm must NOT be rewritten into a Cancel.
// ConfirmationActivity builds `ActivityResult res; res.isCancelled = false;`
// and hands it over through setResult() — empty data, but explicit.
TEST(NormalizeActivityResult, ExplicitConfirmWithMonostateDataIsNotRewritten) {
  ActivityResult res;
  res.isCancelled = false;
  ActivityResult r = asDeliveredViaSetResult(std::move(res));
  EXPECT_FALSE(normalizeActivityResult(r));
  EXPECT_FALSE(r.isCancelled);
}

TEST(NormalizeActivityResult, ExplicitCancelIsUnchanged) {
  ActivityResult res;
  res.isCancelled = true;
  ActivityResult r = asDeliveredViaSetResult(std::move(res));
  EXPECT_FALSE(normalizeActivityResult(r));
  EXPECT_TRUE(r.isCancelled);
}

TEST(NormalizeActivityResult, TypedDataResultIsUnchanged) {
  ActivityResult res(FilePathResult{"/books/some.epub"});
  res.isCancelled = false;
  // A payload-constructed result is explicit even before setResult() runs.
  EXPECT_TRUE(res.hasResult);
  ActivityResult r = asDeliveredViaSetResult(std::move(res));
  EXPECT_FALSE(normalizeActivityResult(r));
  EXPECT_FALSE(r.isCancelled);
  EXPECT_TRUE(std::holds_alternative<FilePathResult>(r.data));
}
