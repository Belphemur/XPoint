// Host tests for TtfWordSelect::buildTtfWordSelectData — token extraction
// from engine run geometry with a real TtfFont (Amazon Ember fixture), the
// seam behind the restored TTF dictionary entry point (PR #113 §17).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

#include "TtfWordSelect.h"
#include "render/TtfFont.h"

namespace {

const std::string& emberBytes() {
  static const std::string bytes = [] {
    std::ifstream f(EMBER_FIXTURES_DIR "/Amazon_Ember_Regular.ttf", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }();
  return bytes;
}

freeink::book::Page makePage(const char* text, const uint16_t len) {
  static freeink::book::PageTextRun run{};
  run.text = text;
  run.charStart = 0;
  run.len = len;
  run.x = 10;
  run.baselineY = 40;
  run.sizePx = 20;
  run.charLen = len;
  run.styleFlags = freeink::book::StyleNone;
  run.layoutFlags = 0;
  static freeink::book::Page page{};
  page.runs = &run;
  page.runCount = 1;
  return page;
}

}  // namespace

TEST(TtfWordSelect, SplitsRunsIntoWhitespaceTokens) {
  static const char kText[] = "alpha beta";
  const auto page = makePage(kText, static_cast<uint16_t>(std::strlen(kText)));
  static uint8_t arena[64 * 1024];
  freeink::book::Arena glyphArena(arena, sizeof(arena));
  freeink::book::TtfFont font;
  const std::string& bytes = emberBytes();
  if (bytes.empty()) GTEST_SKIP() << "Missing Amazon Ember fixture";
  ASSERT_GE(bytes.size(), 16u);
  ASSERT_TRUE(
      font.init(reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<uint32_t>(bytes.size()), glyphArena));
  freeink::book::FontChain chain;
  chain.add(&font, freeink::book::StyleNone);

  freeink::book::TtfWordSelectData data;
  ASSERT_TRUE(freeink::book::buildTtfWordSelectData(page, chain, data));
  ASSERT_EQ(data.boxes.size(), 2u);
  EXPECT_STREQ(data.boxes[0].text, "alpha");
  EXPECT_STREQ(data.boxes[1].text, "beta");
  EXPECT_EQ(data.boxes[0].textOffset, 0u);
  EXPECT_EQ(data.boxes[0].textLength, 5u);
  EXPECT_EQ(data.boxes[0].rawLength, 5u);
  // textOffset/textLength address the trim range INSIDE the copied token;
  // clean tokens have nothing to trim (offset 0, length == rawLength).
  EXPECT_EQ(data.boxes[1].textOffset, 0u);
  EXPECT_EQ(data.boxes[1].textLength, 4u);
  EXPECT_EQ(data.boxes[1].rawLength, 4u);
  // Geometry: y is the line-box top (baseline - ascent), boxes advance in x.
  EXPECT_LT(data.boxes[0].x, data.boxes[1].x);
  EXPECT_EQ(data.boxes[0].height, chain.lineHeight(20));
}
