#pragma once

// StyleBits — the §3.5 item 9 translation table between the legacy
// EpdFontFamily::Style bitmask and the engine's StyleFlags. Only BOLD/ITALIC
// (+ UNDERLINE's overlay bit position) coincide; the legacy STRIKETHROUGH/SUP/
// SUB/RUBY_CONTINUE bits collide with the engine's StyleSuperscript/
// StyleSubscript, so a cast would render strikethrough text as superscript.
// The static_asserts pin the coinciding bits against engine drift.

#include <cstdint>

#include <BookFont.h>

#include <EpdFontFamily.h>

namespace freeink {
namespace book {

static_assert(StyleBold == EpdFontFamily::BOLD, "bold bit must coincide");
static_assert(StyleItalic == EpdFontFamily::ITALIC, "italic bit must coincide");
static_assert(StyleUnderline == EpdFontFamily::UNDERLINE, "underline bit must coincide");
// The collisions that make a cast unsafe (§5 D8).
static_assert(StyleSuperscript == EpdFontFamily::STRIKETHROUGH,
              "engine sup bit must keep colliding with legacy strikethrough");
static_assert(StyleSubscript == EpdFontFamily::SUP, "engine sub bit must keep colliding with legacy sup");

namespace stylebits {

// Legacy word style → engine StyleFlags. Decorations the engine does not
// carry (legacy STRIKETHROUGH/SUP/SUB/RUBY_CONTINUE) are dropped here: the
// engine draws strikethrough from its own StyleStrikethrough bit (set during
// CSS parse), and sup/sub baselines shift at engine layout time — so the
// legacy overlay bits must never leak into a FontChain request.
inline uint8_t legacyToEngine(const uint8_t legacyStyle) {
  uint8_t out = 0;
  if (legacyStyle & EpdFontFamily::BOLD) out |= StyleBold;
  if (legacyStyle & EpdFontFamily::ITALIC) out |= StyleItalic;
  if (legacyStyle & EpdFontFamily::UNDERLINE) out |= StyleUnderline;
  return out;
}

}  // namespace stylebits
}  // namespace book
}  // namespace freeink