#pragma once

#include <climits>

// Arithmetic behind the C3-class auto-repeat stall discount (see
// docs/design/2026-09-30-auto-repeat-real-hold.md). Pure constexpr policy, no
// state: MappedInputManager owns the accumulators and calls these, and the host
// suite exercises them directly.
namespace repeathold {

// One inter-frame gap above this counts as a render stall rather than a normal
// loop tick. Generous against a 10-50 ms tick so ordinary jitter is never
// charged to a hold window.
constexpr unsigned long kStallThresholdMs = 100;

// The part of an inter-frame gap that is stall time: nothing for a normal tick,
// otherwise everything beyond the threshold (a fresh contact is entitled to its
// own threshold of loop time).
constexpr unsigned long stallFor(unsigned long gapMs) {
  return gapMs > kStallThresholdMs ? gapMs - kStallThresholdMs : 0;
}

// Accumulate a stall saturating instead of capping. Capping is wrong: the
// discount is subtracted from an UNCAPPED wall-clock hold time, so a capped
// discount leaves the remainder as hold time and the same press repeats on the
// first tick after a stall longer than the cap (a multi-second chapter build
// on the C3). Overflow can only be reached by a session with no press edge at
// all, so saturation is the safe bound.
constexpr unsigned long addSaturated(unsigned long accumulatedMs, unsigned long addMs) {
  return accumulatedMs > ULONG_MAX - addMs ? ULONG_MAX : accumulatedMs + addMs;
}

// Hold time left for the repeat gate once stalls are discounted, saturating at
// zero (a stall at least as long as the hold leaves nothing to repeat).
constexpr unsigned long discount(unsigned long heldMs, unsigned long accumulatedStallMs) {
  return heldMs > accumulatedStallMs ? heldMs - accumulatedStallMs : 0;
}

// True when this frame's press edge STARTS a contact and may therefore zero
// the discount. The SDK's held clock is aggregate — it runs from the first
// button down — so a second button pressed while the navigation button is
// still held (or an edge parked across a blocking transfer) must not wipe the
// discount belonging to that still-held contact.
constexpr bool startsNewContact(bool hasPressEdge, bool anyHeldLastFrame) { return hasPressEdge && !anyHeldLastFrame; }

}  // namespace repeathold
