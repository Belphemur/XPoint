#include "PowerStatsOverlay.h"

#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <cstdio>

#include "../../src/fontIds.h"
#include "EnduranceGovernor.h"
#include "WifiLeakGuard.h"

namespace {

// The overlay's own font. UI_10 is the smallest UI face the theme ships, which
// is what lets a 12-row block fit inside the oriented viewable margins in
// portrait without overlapping the page text.
constexpr int kFontId = UI_10_FONT_ID;

// Percent of `total` in tenths of a percent, so the boot/idle split can be
// printed with integer printf. Returns 0 when total is 0.
uint32_t tenthsPercent(uint32_t part, uint32_t total) {
  if (total == 0) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(part) * 1000ULL / total);
}

// Scale a milli-unit into a human string: 330000 -> "330", 45300 -> "45.3".
// Embedded newlib builds do not enable float printf, so every decimal on this
// screen is rendered from integer arithmetic.
void formatMilli(char* out, size_t outLen, uint32_t milli, int decimals) {
  if (decimals == 1) {
    std::snprintf(out, outLen, "%lu.%lum", static_cast<unsigned long>(milli / 1000UL),
                  static_cast<unsigned long>((milli % 1000UL) / 100UL));
  } else if (decimals == 2) {
    std::snprintf(out, outLen, "%lu.%02lum", static_cast<unsigned long>(milli / 1000UL),
                  static_cast<unsigned long>((milli % 1000UL) / 10UL));
  } else if (decimals == 3) {
    std::snprintf(out, outLen, "%lu.%03lum", static_cast<unsigned long>(milli / 1000UL),
                  static_cast<unsigned long>(milli % 1000UL));
  } else {
    std::snprintf(out, outLen, "%lu", static_cast<unsigned long>(milli / 1000UL));
  }
}

const char* profileName(const endurance::Profile profile) {
  switch (profile) {
    case endurance::Profile::Balanced:
      return "Balanced";
    case endurance::Profile::Performance:
      return "Performance";
    case endurance::Profile::Endurance:
    default:
      return "Endurance";
  }
}

}  // namespace

void PowerStatsOverlay::drawCompact(GfxRenderer& renderer, const int statusBarTextY) {
  const EnduranceGovernor& governor = EnduranceGovernor::instance();
  const PowerDrainMonitor& drain = governor.drain();
  const auto& estimate = drain.estimate();

  // ~mA is only honest once the 10-minute gate has opened; before that the
  // compact line shows a dash rather than a rate that is mostly page-turn noise.
  char current[16] = "--";
  if (estimate.measured) formatMilli(current, sizeof(current), estimate.milliAmp, 1);

  char line[ROW_BYTES];
  std::snprintf(line, sizeof(line), tr(STR_PWR_COMPACT_LINE), governor.idleClockMHz(),
                static_cast<unsigned long>(tenthsPercent(governor.napMs(), governor.bootMs()) / 10),
                static_cast<unsigned>(powerManager.getBatteryPercentage()), current,
                static_cast<unsigned long>(governor.pageTurnMs()));

  renderer.drawText(kFontId, 0, statusBarTextY, line);
}

void PowerStatsOverlay::drawFull(GfxRenderer& renderer, const EnduranceGovernor& governor) {
  char row[ROW_BYTES];

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);

  const PowerDrainMonitor& drain = governor.drain();
  const auto& estimate = drain.estimate();
  const auto& sleep = drain.lastSleep();

  const int lineHeight = renderer.getLineHeight(kFontId);
  const int x = marginLeft + 4;
  const int y = marginTop + 4;

  // Clear a block behind the rows so the block stays legible over page text.
  // The height is computed from the longest row the block can produce, so the
  // white-out always covers every row it is about to draw.
  const int blockWidth = renderer.getScreenWidth() - marginLeft - marginRight - 8;
  const int blockHeight = lineHeight * MAX_ROWS;
  renderer.fillRect(x - 4, y - 2, blockWidth + 8, blockHeight, false);
  renderer.drawRect(x - 4, y - 2, blockWidth + 8, blockHeight, 1, true);

  int rowIndex = 0;
  auto emit = [&](const char* text) {
    if (rowIndex >= MAX_ROWS) return;  // hard budget: never overrun the block
    renderer.drawText(kFontId, x, y + rowIndex * lineHeight, text);
    rowIndex++;
  };

  std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_HEADER), profileName(governor.profile()),
                governor.heavyJobActive() ? tr(STR_YES) : tr(STR_NO));
  emit(row);

  std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_REST), governor.idleClockMHz(), governor.wakeVerdictText());
  emit(row);

  std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_CLOCKS), governor.idleClockMHz(), governor.bootClockMHz(),
                governor.renderClockMHz(), governor.idlePollSlices() ? tr(STR_YES) : tr(STR_NO));
  emit(row);

  std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_SINCE_BOOT),
                static_cast<unsigned long>(tenthsPercent(governor.napMs(), governor.bootMs()) / 10),
                static_cast<unsigned long>(tenthsPercent(governor.idleMs(), governor.bootMs()) / 10));
  emit(row);

  std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_NAPS), static_cast<unsigned long>(governor.naps()));
  emit(row);

  std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_BATT), static_cast<unsigned>(powerManager.getBatteryPercentage()),
                estimate.onUsbPower ? tr(STR_STATE_ON_USB) : tr(STR_STATE_ON_BATTERY));
  emit(row);

  if (estimate.onUsbPower) {
    std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_DRAIN_USB));
    emit(row);
  } else if (!estimate.measured) {
    // The 10-minute gate (design doc §3.2): minutes of window collected so far.
    std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_DRAIN_MEASURING),
                  static_cast<unsigned long>(estimate.windowMs / 60000UL),
                  static_cast<unsigned long>(PowerDrainMonitor::WINDOW_MINUTES));
    emit(row);
  } else {
    char perHour[20];
    char milliamps[20];
    formatMilli(perHour, sizeof(perHour), estimate.milliPctPerHour, 2);
    formatMilli(milliamps, sizeof(milliamps), estimate.milliAmp, 1);
    std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_DRAIN), perHour, milliamps);
    emit(row);

    // A zero drain over a full window yields a zero rate; dividing by it would
    // report an infinite runtime, so the estimate row is skipped instead.
    if (estimate.milliAmp > 0) {
      char left[32];
      PowerDrainMonitor::formatDuration(left, sizeof(left),
                                        PowerDrainMonitor::runtimeLeftMinutes(powerManager.getBatteryPercentage(),
                                                                                estimate.milliAmp));
      std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_EST), milliamps, left);
      emit(row);
    }
  }

  if (sleep.valid) {
    char duration[32];
    char dropped[20];
    char perHour[20];
    PowerDrainMonitor::formatDuration(duration, sizeof(duration), sleep.durationMs / 60000UL);
    formatMilli(dropped, sizeof(dropped), (static_cast<uint32_t>(sleep.startPct) - sleep.endPct) * 1000UL, 2);
    if (sleep.rateable) {
      formatMilli(perHour, sizeof(perHour), sleep.milliPctPerHour, 3);
      std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_SLEEP), duration, dropped, perHour);
    } else {
      // Sub-minute sleeps do not have enough gauge samples to divide by.
      std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_SLEEP_SHORT), duration);
    }
    emit(row);
  }

  std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_PAGE), static_cast<unsigned long>(governor.pageTurnMs()),
                static_cast<unsigned long>(governor.pageRenderMs()),
                static_cast<unsigned long>(governor.panelRefreshMs()));
  emit(row);

  std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_RADIO), WifiLeakGuard::sessionActive() ? tr(STR_YES) : tr(STR_NO),
                static_cast<unsigned long>(WifiLeakGuard::leaksStopped()));
  emit(row);

  std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_STRATEGY), static_cast<unsigned>(governor.strategyIndex()),
                governor.strikes().toByte() != 0 ? tr(STR_YES) : tr(STR_NO));
  emit(row);
}

void PowerStatsOverlay::draw(GfxRenderer& renderer, const Mode mode, const int statusBarTextY,
                             const EnduranceGovernor& governor) {
  switch (mode) {
    case Mode::Compact:
      drawCompact(renderer, statusBarTextY);
      return;
    case Mode::Full:
      drawFull(renderer, governor);
      return;
    case Mode::Off:
    default:
      return;
  }
}
