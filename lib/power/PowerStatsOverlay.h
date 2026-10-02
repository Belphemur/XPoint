#pragma once

#include <PowerDrainMonitor.h>

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

  // The Compact line needs its own capacity: ROW_BYTES is the *Full block's*
  // per-row budget, and the compact format (clock + nap% + volts + battery% +
  // mA + page time) exceeds 47 characters once the numbers are multi-digit, so
  // reusing it truncated the trailing `pg` field.
  //
  // Worst case for `rest %dMHz nap %lu%% | %s %u%% | ~%smA | pg %lums`, with
  // every field at its widest representable value:
  //   "rest 240MHz nap 100% | 4.21V 100% | ~1100.0mA | pg 4294967295ms" = 63 chars
  // plus the NUL. Locales with longer unit/label text have headroom, and
  // snprintf still truncates safely rather than overrunning.
  static constexpr int COMPACT_BYTES = 96;
  static_assert(COMPACT_BYTES > 64, "compact line must fit its widest field set");

  // Number of rows drawFull() always emits. The row set is deliberately
  // unconditional — drain, runtime-left and last-sleep each print a placeholder
  // when their data is absent — because the reader reserves this many rows of
  // vertical space for the block and that reservation feeds the layout viewport.
  // A data-dependent height would re-paginate the book every time a measurement
  // appeared, and would invalidate the section cache mid-read.
  static constexpr int kFullRowCount = 12;

  // Compact line, built into a caller-supplied buffer. It is NOT drawn here:
  // BaseTheme::drawStatusBar() has already painted its clusters on the same
  // baseline, so a second draw at a guessed x would overprint them. Instead the
  // reader hands this string to the theme, which lays it out as a status-bar
  // element alongside the battery/progress clusters.
  static const char* buildCompact(char* out, size_t outLen);

  // Vertical pixels the Full block occupies at the top of the screen, or 0 when
  // the mode is Off/Compact. Readers add this to their content top margin so the
  // block sits ABOVE the text instead of covering it. Uses the renderer's own
  // line height, so it is correct in every orientation.
  //
  // `bandAbovePx` is the height already consumed at the top of the screen by
  // chrome that must stay visible (a Top-positioned XTC status bar). The block is
  // pushed below it and the reserve grows to match, so the two never overlap.
  static int topReservePx(const GfxRenderer& renderer, Mode mode, int bandAbovePx = 0);

  // Pure half of topReservePx(), host-testable without a GfxRenderer: the block
  // is kFullRowCount rows of `lineHeightPx`, plus its 2px border on each side,
  // pushed down by whatever chrome band already sits at the top.
  static constexpr int blockHeightPx(int lineHeightPx, int bandAbovePx) {
    return bandAbovePx + kFullRowCount * lineHeightPx + 4;
  }

  // Full text block, anchored at the top of the oriented viewable area, above
  // the reading content. The reader reserves topReservePx() for it.
  static void drawFull(GfxRenderer& renderer, const EnduranceGovernor& governor, int bandAbovePx = 0);

  // Outer rectangle the Full block occupies, in oriented screen pixels.
  struct BlockRect {
    int x;
    int y;
    int w;
    int h;
  };
  // Exposed because a caller that composites grayscale planes must exclude this
  // area from its own masks: the gray cells are built from the source image alone,
  // and driving them over the block washes the telemetry out.
  static BlockRect fullBlockRect(const GfxRenderer& renderer, int bandAbovePx = 0);

  // Full text block, anchored at the top of the oriented viewable area, above
  // the reading content. The reader reserves topReservePx() for it.
  // (declared above with drawFull's signature)

  // Dispatch on mode. Full is drawn by the overlay; Compact is only formatted
  // here — the caller passes the result into the theme's status bar.
  static void draw(GfxRenderer& renderer, Mode mode, const EnduranceGovernor& governor, int bandAbovePx = 0);

  // Verdict text for the overlay rows, translated. The governor's own
  // wakeVerdictText() stays English because it is only used in log lines.
  static const char* verdictText(const EnduranceGovernor& governor);

  // Duration labels for PowerDrainMonitor::formatDuration(), translated.
  static PowerDrainMonitor::DurationLabels durationLabels();

  // Clamp a stored settings byte. A corrupt/migrated value must never select a
  // mode outside the enum.
  static Mode clampMode(uint8_t raw) {
    return raw <= static_cast<uint8_t>(Mode::Full) ? static_cast<Mode>(raw) : Mode::Off;
  }
};
