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
// screen is rendered from integer arithmetic. The unit is NOT appended here —
// every caller's translated format string already carries it (mA, %/h, %), so a
// suffix would render as "32.8mmA".
void formatMilli(char* out, size_t outLen, uint32_t milli, int decimals) {
  const unsigned long whole = static_cast<unsigned long>(milli / 1000UL);
  const unsigned long frac = static_cast<unsigned long>(milli % 1000UL);
  if (decimals == 1) {
    std::snprintf(out, outLen, "%lu.%lu", whole, frac / 100UL);
  } else if (decimals == 2) {
    std::snprintf(out, outLen, "%lu.%02lu", whole, frac / 10UL);
  } else if (decimals == 3) {
    std::snprintf(out, outLen, "%lu.%03lu", whole, frac);
  } else {
    std::snprintf(out, outLen, "%lu", whole);
  }
}

// Profile names go through tr() like every other user-facing string: the
// overlay header is rendered on-screen, so a hard-coded English name would leave
// Profile names go through tr() like every other user-facing string: the
// overlay header is rendered on-screen, so a hard-coded English name would leave
// this row untranslated in every other locale.
const char* profileName(const endurance::Profile profile) {
  switch (profile) {
    case endurance::Profile::Balanced:
      return tr(STR_PROFILE_BALANCED);
    case endurance::Profile::Performance:
      return tr(STR_PROFILE_PERFORMANCE);
    case endurance::Profile::Endurance:
    default:
      return tr(STR_PROFILE_ENDURANCE);
  }
}

}  // namespace

const char* PowerStatsOverlay::buildCompact(char* out, const size_t outLen) {
  const EnduranceGovernor& governor = EnduranceGovernor::instance();
  const PowerDrainMonitor& drain = governor.drain();
  const auto& estimate = drain.estimate();

  // ~mA is only honest once the 10-minute gate has opened; before that the
  // compact line shows a dash rather than a rate that is mostly page-turn noise.
  char current[16] = "--";
  // A window collected on external power is not battery current; Full says so
  // explicitly, so Compact must suppress the value the same way.
  if (estimate.measured && !estimate.onUsbPower) formatMilli(current, sizeof(current), estimate.milliAmp, 1);

  // Pack voltage, the reference Crossfire line's second segment. Built from
  // millivolts with integer maths because embedded newlib has no float printf.
  // A board with no voltage path shows `--V` rather than a plausible-looking 0V.
  char volts[12] = "--V";
  uint16_t millivolts = 0;
  if (powerManager.getBatteryMillivolts(millivolts)) {
    std::snprintf(volts, sizeof(volts), "%u.%02uV", static_cast<unsigned>(millivolts / 1000U),
                  static_cast<unsigned>((millivolts % 1000U) / 10U));
  }

  // pageTurnMs() is a cumulative total across every render, so printing it raw
  // made `pg` grow on each page turn instead of reporting a duration. The
  // average is what the label means (Full mode already divides the same way).
  const unsigned long renders = governor.pageRenders();
  const unsigned long avgPageMs = renders == 0 ? 0 : governor.pageTurnMs() / renders;

  std::snprintf(out, outLen, tr(STR_PWR_COMPACT_LINE), governor.idleClockMHz(),
                static_cast<unsigned long>(tenthsPercent(governor.napMs(), governor.bootMs()) / 10), volts,
                static_cast<unsigned>(powerManager.getBatteryPercentage()), current, avgPageMs);
  return out;
}

int PowerStatsOverlay::topReservePx(const GfxRenderer& renderer, const Mode mode, const int bandAbovePx) {
  // The mode is a parameter, not a SETTINGS read: lib/power must not depend on
  // src/CrossPointSettings.h, and callers already hold the clamped value.
  if (mode != Mode::Full) {
    return 0;
  }
  // The +4 matches the block's own 2px border on each side, so the first text
  // line below the block is not flush against its frame.
  return blockHeightPx(renderer.getLineHeight(kFontId), bandAbovePx);
}

PowerStatsOverlay::BlockRect PowerStatsOverlay::fullBlockRect(const GfxRenderer& renderer, const int bandAbovePx) {
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int lineHeight = renderer.getLineHeight(kFontId);
  // Same arithmetic the fill/outline use: the border sits 4px left and 2px above
  // the text origin and spans the full row width.
  return BlockRect{marginLeft, marginTop + 2 + bandAbovePx,
                   static_cast<int>(renderer.getScreenWidth()) - marginLeft - marginRight, kFullRowCount * lineHeight};
}

void PowerStatsOverlay::drawFull(GfxRenderer& renderer, const EnduranceGovernor& governor, const int bandAbovePx) {
  char row[ROW_BYTES];

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);

  const PowerDrainMonitor& drain = governor.drain();
  const auto& estimate = drain.estimate();
  const auto& sleep = drain.lastSleep();

  const int lineHeight = renderer.getLineHeight(kFontId);
  const int x = marginLeft + 4;
  // Top-anchored, below any chrome band the reader already reserved at the top
  // (a Top-positioned XTC status bar). Nothing here uses a screen-height offset:
  // the block never moves to the bottom in any orientation.
  const int y = marginTop + 4 + bandAbovePx;

  // Two passes over an identical builder keeps the white-out sized to what is
  // drawn without buffering 20 rows of text on the stack.
  bool drawing = false;
  int rowIndex = 0;
  auto emit = [&](const char* text) {
    if (rowIndex >= MAX_ROWS) return;  // hard budget: never overrun the block
    if (drawing) {
      renderer.drawText(kFontId, x, y + rowIndex * lineHeight, text);
    }
    rowIndex++;
  };

  auto build = [&](bool doDraw) {
    drawing = doDraw;
    rowIndex = 0;

    std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_HEADER), profileName(governor.profile()),
                  governor.heavyJobActive() ? tr(STR_YES) : tr(STR_NO));
    emit(row);

    std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_REST), governor.idleClockMHz(), verdictText(governor));
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

    // Drain and runtime-left are emitted unconditionally, each with a placeholder
    // when its data is absent. Same for the sleep row below. The block must be
    // exactly kFullRowCount rows tall at all times, because readers reserve that
    // many rows of content area for it: a row that appeared only once a
    // measurement arrived would change the viewport mid-read and re-paginate the
    // book (and invalidate the section cache) out from under the reader.
    char milliamps[20] = "--";
    if (estimate.measured && !estimate.onUsbPower) {
      formatMilli(milliamps, sizeof(milliamps), estimate.milliAmp, 1);
    }

    if (estimate.onUsbPower) {
      std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_DRAIN_USB));
    } else if (!estimate.measured) {
      // The 10-minute gate (design doc §3.2): minutes of window collected so far.
      std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_DRAIN_MEASURING),
                    static_cast<unsigned long>(estimate.windowMs / 60000UL),
                    static_cast<unsigned long>(PowerDrainMonitor::WINDOW_MINUTES));
    } else {
      char perHour[20];
      formatMilli(perHour, sizeof(perHour), estimate.milliPctPerHour, 2);
      std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_DRAIN), perHour, milliamps);
    }
    emit(row);

    {
      // A zero drain over a full window yields a zero rate; dividing by it would
      // report an infinite runtime, so the estimate prints its placeholder.
      char left[32] = "--";
      if (estimate.measured && !estimate.onUsbPower && estimate.milliAmp > 0) {
        PowerDrainMonitor::formatDuration(
            left, sizeof(left),
            PowerDrainMonitor::runtimeLeftMinutes(powerManager.getBatteryPercentage(), estimate.milliAmp),
            durationLabels());
      }
      std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_EST), milliamps, left);
      emit(row);
    }

    if (sleep.valid) {
      char duration[32];
      char dropped[20];
      char perHour[20];
      PowerDrainMonitor::formatDuration(duration, sizeof(duration), sleep.durationMs / 60000UL, durationLabels());
      formatMilli(dropped, sizeof(dropped), (static_cast<uint32_t>(sleep.startPct) - sleep.endPct) * 1000UL, 2);
      if (sleep.rateable) {
        formatMilli(perHour, sizeof(perHour), sleep.milliPctPerHour, 3);
        std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_SLEEP), duration, dropped, perHour);
      } else {
        // Sub-minute sleeps do not have enough gauge samples to divide by.
        std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_SLEEP_SHORT), duration);
      }
    } else {
      std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_SLEEP_NONE));
    }
    emit(row);

    // Two conversions in the format string, so two arguments: the average page
    // turn and the sample count it was averaged over. Supplying only the average
    // made snprintf read a nonexistent variadic argument.
    const unsigned long pageRenders = governor.pageRenders();
    std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_PAGE),
                  static_cast<unsigned long>(governor.pageTurnMs() / (pageRenders ? pageRenders : 1u)), pageRenders);
    emit(row);

    std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_RADIO), WifiLeakGuard::sessionActive() ? tr(STR_YES) : tr(STR_NO),
                  static_cast<unsigned long>(WifiLeakGuard::leaksStopped()));
    emit(row);

    std::snprintf(row, sizeof(row), tr(STR_PWR_FULL_STRATEGY), static_cast<unsigned>(governor.strategyIndex()),
                  governor.strikes().toByte() != 0 ? tr(STR_YES) : tr(STR_NO));
    emit(row);
  };

  build(false);
  const int drawnRows = rowIndex;

  // The readers subtract kFullRowCount rows from the content viewport, so the
  // builder must emit exactly that many. emit() clamps at MAX_ROWS, and every row
  // here is unconditional, so a mismatch can only come from an edit that added or
  // removed a row without updating kFullRowCount — which would silently overlap
  // the page text or leave dead space.
  if (drawnRows != kFullRowCount) {
    LOG_ERR("PWR", "Full block emitted %d rows but %d are reserved", drawnRows, kFullRowCount);
  }

  const int blockWidth = renderer.getScreenWidth() - marginLeft - marginRight - 8;
  const int blockHeight = drawnRows * lineHeight;
  renderer.fillRect(x - 4, y - 2, blockWidth + 8, blockHeight, false);
  renderer.drawRect(x - 4, y - 2, blockWidth + 8, blockHeight, 1, true);

  build(true);
}

const char* PowerStatsOverlay::verdictText(const EnduranceGovernor& governor) {
  return I18N.get(governor.wakeVerdict() == EnduranceGovernor::WakeVerdict::Verified ? StrId::STR_PWR_VERDICT_VERIFIED
                  : governor.wakeVerdict() == EnduranceGovernor::WakeVerdict::DemotedToPoll
                      ? StrId::STR_PWR_VERDICT_POLL
                      : StrId::STR_PWR_VERDICT_UNVERIFIED);
}

// Resolved on every call, never cached: tr() at namespace scope would run during
// static initialisation, before setup() calls I18N.setLanguage(), and freeze the
// units to English for the whole boot — and would not follow a runtime language
// change either.
PowerDrainMonitor::DurationLabels PowerStatsOverlay::durationLabels() {
  return {tr(STR_PWR_DUR_LT_MIN), tr(STR_PWR_DUR_MIN), tr(STR_PWR_DUR_HM), tr(STR_PWR_DUR_DH)};
}

void PowerStatsOverlay::draw(GfxRenderer& renderer, const Mode mode, const EnduranceGovernor& governor,
                             const int bandAbovePx) {
  switch (mode) {
    case Mode::Compact:
      // Formatted by the caller into the theme's status bar; nothing to draw here.
      return;
    case Mode::Full:
      drawFull(renderer, governor, bandAbovePx);
      return;
    case Mode::Off:
    default:
      return;
  }
}
