#include <gtest/gtest.h>

#include <cstdint>

#include "Epub/ImageStaging.h"

namespace {

TEST(ImageStagingGuard, AcceptsImageAtTheLimit) { EXPECT_TRUE(isImageSizeWithinPsramLimit(MAX_IMAGE_FILE_SIZE)); }

TEST(ImageStagingGuard, RejectsImageAboveTheLimit) {
  EXPECT_FALSE(isImageSizeWithinPsramLimit(MAX_IMAGE_FILE_SIZE + 1));
  EXPECT_FALSE(isImageSizeWithinPsramLimit(SIZE_MAX));
}

}  // namespace
