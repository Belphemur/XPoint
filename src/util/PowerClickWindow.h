#pragma once

#include <cstdint>

// Pure decision policy for the X4 Pro frontlight double-click window
// (soak-fix7 JFhK; audit docs/design/2026-09-22-input-coverage.md): the first
// short Power release is ambiguous (frontlight double-click vs the configured
// short-power action) and is held until the window resolves. Header-only, no
// dependencies: MappedInputManager feeds it this tick's snapshot state and
// applies the result; host tests drive the policy directly
// (test/input_grammar/).
class PowerClickWindow {
 public:
  static constexpr uint32_t kDoubleClickWindowMs = 500;
  static constexpr uint32_t kClickMaxHoldMs = 300;

  enum class Verdict : uint8_t {
    Arm,          // first short click: open the window, hold the release
    DoubleClick,  // second short click inside the window
    Confirm,      // hold past the click window but inside the Confirm carve-out
    Deliver,      // deliver the release to the Power handlers
  };

  struct Result {
    bool serveRelease = false;  // the Power release bit ends this tick SERVED
    bool holdRelease = false;   // the tick's physical release is held in a (new) window
    bool confirmEdge = false;   // raise the per-tick Confirm verdict
    bool doubleClick = false;   // raise the frontlight double-click verdict
  };

  // Open/closed is explicit: start is a raw millis() value, and 0 is a legal
  // timestamp (boot start and the 49.7-day wrap), so it cannot double as the
  // closed sentinel (qodo T3).
  struct WindowState {
    bool open = false;
    uint32_t start = 0;
  };

  static bool expired(const WindowState& state, const uint32_t now) {
    return state.open && now - state.start > kDoubleClickWindowMs;
  }

  // Classify one Power release. secondClick: another release is already
  // parked in the window. confirmHoldMs: the PWR_CONFIRM carve-out threshold
  // (0 = carve-out disabled). heldMs: the Power hold duration of THIS release.
  static Verdict classify(const bool secondClick, const uint32_t heldMs, const uint32_t confirmHoldMs) {
    if (heldMs <= kClickMaxHoldMs) return secondClick ? Verdict::DoubleClick : Verdict::Arm;
    if (confirmHoldMs != 0 && heldMs <= confirmHoldMs) return Verdict::Confirm;
    return Verdict::Deliver;
  }

  // One window tick. physicalRelease: a Power release edge is in this tick's
  // snapshot; comboRelease: a Down release edge accompanies it — the screenshot
  // combo is ending, whose Power release is inert (never armed, always served).
  // Updates state in place.
  static Result tick(WindowState& state, const bool physicalRelease, const bool comboRelease, const uint32_t now,
                     const uint32_t heldMs, const uint32_t confirmHoldMs) {
    Result out;
    if (expired(state, now)) {
      // The held release resolves: delivered now, with the Confirm edge the
      // PWR_CONFIRM shortcut keys off. A physical release on the same tick
      // classifies on its own — a short click re-arms for its own window
      // (a double-click spanning the expiry boundary) instead of merging
      // into the deferred delivery.
      state.open = false;
      out.serveRelease = true;
      out.confirmEdge = true;
      if (physicalRelease && !comboRelease) {
        switch (classify(false, heldMs, confirmHoldMs)) {
          case Verdict::Arm:
            state.open = true;
            state.start = now;
            out.holdRelease = true;
            break;
          case Verdict::Confirm:
            out.confirmEdge = true;
            break;
          case Verdict::Deliver:
            out.serveRelease = true;
            break;
          case Verdict::DoubleClick:
            break;  // unreachable: secondClick is false here
        }
      }
      return out;
    }
    if (!physicalRelease) return out;
    if (comboRelease) {
      // The screenshot combo's Power release is not a short-power click:
      // serve it and drop any window state.
      state.open = false;
      out.serveRelease = true;
      return out;
    }
    switch (classify(state.open, heldMs, confirmHoldMs)) {
      case Verdict::Arm:
        state.open = true;
        state.start = now;
        out.holdRelease = true;
        break;
      case Verdict::DoubleClick:
        state.open = false;
        out.doubleClick = true;
        break;
      case Verdict::Confirm:
        state.open = false;
        out.confirmEdge = true;
        break;
      case Verdict::Deliver:
        state.open = false;
        out.serveRelease = true;
        break;
    }
    return out;
  }
};
