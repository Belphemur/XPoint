#pragma once

#include <stddef.h>

// Pre-allocation guard for a single compressed image on PSRAM boards.
// Images above this limit take the legacy SD staging path.
inline constexpr size_t MAX_IMAGE_FILE_SIZE = 4 * 1024 * 1024;

constexpr bool isImageSizeWithinPsramLimit(size_t size) { return size <= MAX_IMAGE_FILE_SIZE; }