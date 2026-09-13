#include <Memory.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "Epub/ImageStaging.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"

namespace {

TEST(ImageStagingGuard, AcceptsImageAtTheLimit) { EXPECT_TRUE(isImageSizeWithinPsramLimit(MAX_IMAGE_FILE_SIZE)); }

TEST(ImageStagingGuard, RejectsImageAboveTheLimit) {
  EXPECT_FALSE(isImageSizeWithinPsramLimit(MAX_IMAGE_FILE_SIZE + 1));
  EXPECT_FALSE(isImageSizeWithinPsramLimit(SIZE_MAX));
}

// Fallback decision seam: the caller treats "no PSRAM buffer" (oversized or
// OOM) as the signal to use the legacy SD path. These tests pin the decision
// boundary: a null buffer always means "fall back", whatever the reason.
TEST(PsramStagingDecision, NullBufferMeansFallback) {
  // Simulates Epub::extractItemToPsram returning nullptr: oversized, OOM, or
  // missing item all collapse to the same fallback decision.
  PoolBytes empty{nullptr};
  const bool usePsramPath = static_cast<bool>(empty);
  EXPECT_FALSE(usePsramPath);
}

TEST(PsramStagingDecision, NonNullBufferMeansPsramPath) {
  std::array<uint8_t, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
  HalMemoryFile view;
  view.attach(bytes.data(), bytes.size());
  const bool usePsramPath = view.isOpen();
  EXPECT_TRUE(usePsramPath);
}

// HalMemoryFile read/seek surface: enough of the HalFile-shaped API for the
// PNG BMP chunk walker.
TEST(HalMemoryFileTest, ReadsAndSeeksAcrossTheBuffer) {
  std::array<uint8_t, 16> bytes{};
  for (size_t i = 0; i < bytes.size(); i++) bytes[i] = static_cast<uint8_t>(i);

  HalMemoryFile view;
  view.attach(bytes.data(), bytes.size());
  ASSERT_TRUE(view.isOpen());
  EXPECT_EQ(view.size(), 16u);

  uint8_t out[4] = {};
  EXPECT_EQ(view.read(out, 4), 4);
  EXPECT_EQ(out[0], 0);
  EXPECT_EQ(out[3], 3);
  EXPECT_EQ(view.position(), 4u);

  EXPECT_TRUE(view.seek(12));
  EXPECT_EQ(view.read(out, 4), 4);
  EXPECT_EQ(out[0], 12);
  EXPECT_EQ(view.position(), 16u);

  // Short read at EOF: returns fewer bytes than requested, not an error.
  EXPECT_EQ(view.read(out, 4), 0);

  // Seek beyond the buffer fails; seekCur can walk back into range.
  EXPECT_FALSE(view.seek(17));
  EXPECT_TRUE(view.seekCur(-4));
  EXPECT_EQ(view.position(), 12u);
}

TEST(HalMemoryFileTest, UnattachedViewFailsReads) {
  HalMemoryFile view;
  uint8_t out[4] = {};
  EXPECT_EQ(view.read(out, 4), -1);
  EXPECT_FALSE(view.isOpen());
  EXPECT_FALSE(static_cast<bool>(view));
}

TEST(HalMemoryFileTest, ReadAtEndOfBufferReturnsZeroWithoutOverflow) {
  std::array<uint8_t, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
  HalMemoryFile view;
  view.attach(bytes.data(), bytes.size());
  ASSERT_TRUE(view.seek(8));  // exactly at end
  uint8_t out[4] = {};
  // pos_ == size_: subtraction-form bound check returns 0, no wraparound.
  EXPECT_EQ(view.read(out, 4), 0);
  EXPECT_EQ(view.position(), 8u);
}

}  // namespace
