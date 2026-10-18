#pragma once

// Host-test stub: Section.cpp releases SD font caches at teardown and
// ImageBlock.cpp skips cache rendering while a scan is running. The fixture
// never scans, so both are no-ops. Keep in sync with
// lib/GfxRenderer/FontCacheManager.h.
class FontCacheManager {
 public:
  void releaseSdFontCaches() {}
  bool isScanning() const { return false; }
};
