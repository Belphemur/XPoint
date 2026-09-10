#include "FrameTargetFactory.h"

#include <CrossPointSettings.h>

namespace book = freeink::book;

// The renderer's Orientation is the settings ORIENTATION in the same 0..3
// order, but the engine's FrameRotation enum (None, Portrait,
// PortraitInverted, UpsideDown) deliberately does NOT share that order —
// map through the explicit table below.
static_assert(static_cast<uint8_t>(GfxRenderer::Portrait) == CrossPointSettings::PORTRAIT);
static_assert(static_cast<uint8_t>(GfxRenderer::LandscapeClockwise) == CrossPointSettings::LANDSCAPE_CW);
static_assert(static_cast<uint8_t>(GfxRenderer::PortraitInverted) == CrossPointSettings::INVERTED);
static_assert(static_cast<uint8_t>(GfxRenderer::LandscapeCounterClockwise) == CrossPointSettings::LANDSCAPE_CCW);

book::FrameTarget makeFrameTarget(const GfxRenderer& renderer) {
  book::FrameTarget target{};
  target.framebuffer = renderer.getFrameBuffer();
  target.width = static_cast<int16_t>(renderer.getDisplayWidth());  // panel-native
  target.height = static_cast<int16_t>(renderer.getDisplayHeight());
  target.widthBytes = static_cast<int16_t>(renderer.getDisplayWidthBytes());
  target.format = SETTINGS.textAntiAliasing ? book::FrameFormat::Mono1Dithered : book::FrameFormat::Mono1Sharp;

  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait:
      target.rotation = book::FrameRotation::Portrait;  // 90° CW
      break;
    case GfxRenderer::LandscapeClockwise:
      target.rotation = book::FrameRotation::UpsideDown;
      break;
    case GfxRenderer::PortraitInverted:
      target.rotation = book::FrameRotation::PortraitInverted;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      target.rotation = book::FrameRotation::None;  // native panel orientation
      break;
  }
  return target;
}
