#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "FirmwareFlasher.h"
#include "HalStorage.h"

namespace {

// A minimal ESP app image with one segment and no SHA trailer. Its total size
// is the 16-byte aligned body plus the checksum byte, and its embedded board
// tag matches the x4pro board compiled into this host target.
std::string buildValidImage() {
  constexpr uint32_t kSegmentDataLen = 65500;
  constexpr size_t kTotalSize = 65536;  // 24-byte header + 8-byte seg header + data + 4-byte pad
  constexpr char kBoardTag[] = "CROSSPOINT-BOARD-V1:x4pro;";

  std::string image(kTotalSize, '\0');
  image[0] = '\xE9';  // ESP_IMAGE_MAGIC
  image[1] = 1;       // one segment
  constexpr uint16_t kChipId = 0x1234;
  std::memcpy(image.data() + 12, &kChipId, sizeof(kChipId));
  image[23] = 0;  // no appended SHA-256 trailer

  std::memcpy(image.data() + 24 + 4, &kSegmentDataLen, sizeof(kSegmentDataLen));
  std::memcpy(image.data() + 32, kBoardTag, sizeof(kBoardTag) - 1);
  for (size_t i = sizeof(kBoardTag) - 1; i < kSegmentDataLen; ++i) image[32 + i] = '\xA5';

  uint8_t checksum = 0xEF;
  for (uint32_t i = 0; i < kSegmentDataLen; ++i) checksum ^= static_cast<uint8_t>(image[32 + i]);
  image[kTotalSize - 1] = static_cast<char>(checksum);

  return image;
}

}  // namespace

// The SD picker filters by .bin only; validateImageFile must therefore judge
// firmware by its bytes, not by whether its filename follows a release-asset
// convention.
TEST(FirmwareFlasherValidateImageFile, AcceptsAnyBinFilenameForIdenticalImageBytes) {
  const std::string image = buildValidImage();
  constexpr size_t kPartitionSize = 2 * 1024 * 1024;
  auto& files = Storage.files;
  files["/xpoint-1.16.0-x4pro.bin"] = image;
  files["/crosspoint-1.16.0-x4pro.bin"] = image;
  files["/locally-built-firmware.bin"] = image;

  EXPECT_EQ(firmware_flash::validateImageFile("/xpoint-1.16.0-x4pro.bin", kPartitionSize), firmware_flash::Result::OK);
  EXPECT_EQ(firmware_flash::validateImageFile("/crosspoint-1.16.0-x4pro.bin", kPartitionSize),
            firmware_flash::Result::OK);
  EXPECT_EQ(firmware_flash::validateImageFile("/locally-built-firmware.bin", kPartitionSize),
            firmware_flash::Result::OK);
}
