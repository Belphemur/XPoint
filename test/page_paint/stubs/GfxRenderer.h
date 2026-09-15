// Host-test stub of GfxRenderer — just the surface PagePaint consumes, with
// the same band-target semantics as the real renderer (no rotation on host:
// logical == physical coordinates).
#pragma once

#include <cstdint>
#include <cstring>

class GfxRenderer {
 public:
  static constexpr int kPanelW = 64;
  static constexpr int kPanelH = 48;
  static constexpr int kStride = kPanelW / 8;
  static constexpr int kPlaneBytes = kStride * kPanelH;

  int getScreenWidth() const { return kPanelW; }
  int getScreenHeight() const { return kPanelH; }

  void beginStripTarget(uint8_t* scratch, int stripY0, int stripRows, uint8_t* msbScratch) {
    _stripBuf = scratch;
    _dualBuf = msbScratch;
    _stripY0 = stripY0;
    _stripRows = stripRows;
    _stripActive = true;
  }
  void endStripTarget() {
    _stripActive = false;
    _stripBuf = nullptr;
    _dualBuf = nullptr;
    _stripY0 = 0;
    _stripRows = 0;
  }
  bool glyphIntersectsStrip(int x0, int y0, int x1, int y1) const {
    if (!_stripActive) return true;
    return !(y1 < _stripY0 || y0 >= _stripY0 + _stripRows);
  }

  void drawPixel(int x, int y, bool state) const {
    if (!state) return;
    if (x < 0 || y < 0 || x >= kPanelW || y >= kPanelH) return;
    base[y * kStride + x / 8] |= static_cast<uint8_t>(0x80 >> (x & 7));
  }
  void drawGrayDualPixel(int x, int y, bool msb, bool lsb) const {
    if (!msb && !lsb) return;
    if (_dualBuf == nullptr) return;  // real contract: dual writes need an active dual target
    if (y < _stripY0 || static_cast<int64_t>(y) >= static_cast<int64_t>(_stripY0) + static_cast<int64_t>(_stripRows))
      return;
    const int idx = (y - _stripY0) * kStride + x / 8;
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
    if (lsb) _stripBuf[idx] |= mask;
    if (msb) _dualBuf[idx] |= mask;
  }

  // Base-plane sink for the tests (paintText's drawPixel target).
  mutable uint8_t base[kPlaneBytes] = {};

 private:
  mutable uint8_t* _stripBuf = nullptr;
  mutable uint8_t* _dualBuf = nullptr;
  int _stripY0 = 0;
  int _stripRows = 0;
  bool _stripActive = false;
};
