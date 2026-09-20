#pragma once

#include <GfxRenderer.h>

#include "render/PageRenderer.h"

// Builds the engine FrameTarget for the CURRENT renderer state: panel-native
// framebuffer, 1bpp MSB-first, SET=white — the same convention GfxRenderer
// draws with. The format follows the textRenderMode setting.
freeink::book::FrameTarget makeFrameTarget(const GfxRenderer& renderer);
