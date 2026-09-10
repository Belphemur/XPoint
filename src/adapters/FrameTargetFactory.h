#pragma once

#include <GfxRenderer.h>

#include "render/PageRenderer.h"

// Builds the engine FrameTarget for the CURRENT renderer state: panel-native
// framebuffer, 1bpp MSB-first, SET=white — the same convention GfxRenderer
// draws with. The format follows the textAntiAliasing setting.
freeink::book::FrameTarget makeFrameTarget(GfxRenderer& renderer);
