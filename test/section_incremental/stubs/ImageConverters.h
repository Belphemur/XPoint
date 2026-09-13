#pragma once

// Host stub: cover-image conversion is only reached from generateCoverBmp()/
// generateThumbBmp(), which this test never calls. Signatures must line up
// with the call sites in Epub.cpp so it still links; definitions live in
// ParserLinkStubs.cpp.

#include <HalStorage.h>
#include <Memory.h>

class JpegToBmpConverter {
 public:
  static bool jpegFileToBmpStream(HalFile& jpegFile, HalFile& bmpFile, bool cropped, bool originalThresholds = false);
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, HalFile& bmpFile, int targetWidth, int targetHeight);
  static bool jpegMemToBmpStream(uint8_t* jpegData, size_t jpegSize, Print& bmpOut, bool crop = true,
                                 bool originalThresholds = false);
};

class PngToBmpConverter {
 public:
  static bool pngFileToBmpStream(HalFile& pngFile, HalFile& bmpFile, bool cropped, bool originalThresholds = false);
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, HalFile& bmpFile, int targetWidth, int targetHeight);
  static bool pngMemToBmpStream(uint8_t* pngData, size_t pngSize, Print& bmpOut, bool crop = true,
                                bool originalThresholds = false);
};
