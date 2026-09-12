// StyleBits — §3.5 item 9: the legacy→engine style translation must never
// leak legacy overlay bits (a cast would render legacy STRIKETHROUGH=8 text
// as engine StyleSuperscript).
#include <gtest/gtest.h>

#include <cstdint>

#include "adapters/StyleBits.h"

using freeink::book::stylebits::legacyToEngine;

TEST(StyleBits, CoincidingBitsMapThrough) {
  EXPECT_EQ(legacyToEngine(EpdFontFamily::REGULAR), freeink::book::StyleNone);
  EXPECT_EQ(legacyToEngine(EpdFontFamily::BOLD), freeink::book::StyleBold);
  EXPECT_EQ(legacyToEngine(EpdFontFamily::ITALIC), freeink::book::StyleItalic);
  EXPECT_EQ(legacyToEngine(EpdFontFamily::BOLD_ITALIC), freeink::book::StyleBold | freeink::book::StyleItalic);
  EXPECT_EQ(legacyToEngine(EpdFontFamily::BOLD | EpdFontFamily::UNDERLINE),
            static_cast<uint8_t>(freeink::book::StyleBold | freeink::book::StyleUnderline));
}

TEST(StyleBits, CollidingOverlayBitsAreDropped) {
  // Legacy STRIKETHROUGH=8 == engine StyleSuperscript: leaking it would
  // superscript the text; the engine draws strikethrough from its own bit.
  EXPECT_EQ(legacyToEngine(EpdFontFamily::STRIKETHROUGH), 0);
  EXPECT_EQ(legacyToEngine(EpdFontFamily::SUP), 0);                   // == StyleSubscript
  EXPECT_EQ(legacyToEngine(EpdFontFamily::SUB), 0);                   // no engine counterpart
  EXPECT_EQ(legacyToEngine(EpdFontFamily::RUBY_CONTINUE), 0);
  EXPECT_EQ(legacyToEngine(EpdFontFamily::STRIKETHROUGH | EpdFontFamily::UNDERLINE),
            freeink::book::StyleUnderline);  // the decoration survives, the collision does not
}