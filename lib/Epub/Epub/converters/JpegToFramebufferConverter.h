#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "ImageToFramebufferDecoder.h"

class JpegToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsStatic(const std::string& imagePath, ImageDimensions& out);
  static bool getDimensionsStatic(const uint8_t* data, size_t size, ImageDimensions& out);

  bool decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) override;
  bool decodeToFramebuffer(uint8_t* data, size_t size, GfxRenderer& renderer, const RenderConfig& config) override;

  bool getDimensions(const std::string& imagePath, ImageDimensions& dims) const override {
    return getDimensionsStatic(imagePath, dims);
  }

  bool getDimensions(const uint8_t* data, size_t size, ImageDimensions& dims) const override {
    return getDimensionsStatic(data, size, dims);
  }

  static bool supportsFormat(const std::string& extension);
  const char* getFormatName() const override { return "JPEG"; }
};
