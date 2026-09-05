#pragma once

// Host stub: cover-image conversion is only reached from generateCoverBmp()/
// generateThumbBmp(), which this test never calls. Signatures must line up
// with the call sites in Epub.cpp so it still links; definitions live in
// ParserLinkStubs.cpp.

#include <HalStorage.h>

class JpegToBmpConverter {
 public:
  static bool jpegFileToBmpStream(HalFile& jpegFile, HalFile& bmpFile, bool cropped);
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, HalFile& bmpFile, int targetWidth,
                                              int targetHeight);
};

class PngToBmpConverter {
 public:
  static bool pngFileToBmpStream(HalFile& pngFile, HalFile& bmpFile, bool cropped);
  static bool pngFileTo1BitBmpStreamWithSize(HalFile& pngFile, HalFile& bmpFile, int targetWidth,
                                             int targetHeight);
};
