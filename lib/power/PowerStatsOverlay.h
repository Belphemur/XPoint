#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;
class EnduranceGovernor;

// On-screen power telemetry, settings-gated to Off / Compact / Full.
//
// Compact is a single status-bar line. Full is a text block of up to
// MAX_ROWS rows, each at most ROW_BYTES wide — the decompiled Crossfire
// compositor's budget (design doc §3.3), which is also what keeps this
// drawable on the C3, where there is no PSRAM to absorb a larger scratch area.
//
// Neither mode runs a timer. The overlay is repainted only when the page-turn /
// render path calls draw() — a periodic redraw on an e-ink panel would both
// wear the panel and burn the idle clock the governor just dropped.
//
// Design source: docs/design/2026-10-01-endurance-governor-power-stats.md §3.3.

class PowerStatsOverlay {
 public:
  enum class Mode : uint8_t { Off = 0, Compact = 1, Full = 2 };

  // Crossfire's block budget: 20 rows of 0x30 (48) bytes.
  static constexpr int ROW_BYTES = 0x30;
  static constexpr int MAX_ROWS = 20;
  static_assert(ROW_BYTES >= 32, "a full row plus its NUL must fit the decompiled row budget");

  // Compact line, drawn into the status-bar band. statusBarTextY is the same Y
  // BaseTheme::drawStatusBar uses, so the line sits on the bar's text baseline
  // instead of guessing a second layout.
  static void drawCompact(GfxRenderer& renderer, int statusBarTextY);

  // Full text block, drawn over the page's upper-left corner inside the
  // oriented viewable margins.
  static void drawFull(GfxRenderer& renderer, const EnduranceGovernor& governor);

  // Dispatch on mode. `governor` is only read in Full mode; Compact reads the
  // singleton so the reader does not have to thread it through for both modes.
  static void draw(GfxRenderer& renderer, Mode mode, int statusBarTextY, const EnduranceGovernor& governor);

  // Clamp a stored settings byte. A corrupt/migrated value must never select a
  // mode outside the enum.
  static Mode clampMode(uint8_t raw) {
    return raw <= static_cast<uint8_t>(Mode::Full) ? static_cast<Mode>(raw) : Mode::Off;
  }
};
