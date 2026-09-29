#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "Print.h"
#include "TxtToHtml.h"

namespace {

class StringPrint : public Print {
 public:
  std::string str;
  size_t write(uint8_t b) override {
    str.push_back(static_cast<char>(b));
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    str.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }
};

const std::string kHeader =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!-- TXT_CACHE_VERSION: 1 -->\n"
    "<!DOCTYPE html>\n<html>\n<head><title>test</title></head>\n<body>\n";
const std::string kFooter = "\n</body>\n</html>\n";

std::string convert(std::string_view content) {
  StringPrint out;
  EXPECT_TRUE(TxtToHtml::stream("test.txt", content, out));
  return out.str;
}

TEST(TxtToHtmlTest, LeadingSpacesIndentation) {
  EXPECT_EQ(convert("  two spaces"), kHeader + "&#160;&#160;two spaces" + kFooter);
  EXPECT_EQ(convert("    four spaces"), kHeader + "&#160;&#160;&#160;&#160;four spaces" + kFooter);
  EXPECT_EQ(convert("Line 1\n   three spaces"), kHeader + "Line 1<br />&#160;&#160;&#160;three spaces" + kFooter);
}

TEST(TxtToHtmlTest, MidLineConsecutiveSpaces) {
  EXPECT_EQ(convert("One space"), kHeader + "One space" + kFooter);
  EXPECT_EQ(convert("Two  spaces"), kHeader + "Two&#160; spaces" + kFooter);
  EXPECT_EQ(convert("Three   spaces"), kHeader + "Three&#160;&#160; spaces" + kFooter);
  EXPECT_EQ(convert("Four    spaces"), kHeader + "Four&#160;&#160;&#160; spaces" + kFooter);
}

TEST(TxtToHtmlTest, PreservesCacheVersionTagsForBothFormats) {
  StringPrint out;
  ASSERT_TRUE(TxtToHtml::stream("test.MD", "text", out));
  EXPECT_EQ(out.str,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!-- MD_CACHE_VERSION: 1 -->\n"
            "<!DOCTYPE html>\n<html>\n<head><title>test</title></head>\n<body>\ntext" +
                kFooter);
  EXPECT_NE(out.str.find(TxtToHtml::cacheVersionTag("test.MD")), std::string::npos);
  EXPECT_NE(convert("text").find(TxtToHtml::cacheVersionTag("test.txt")), std::string::npos);
}

TEST(TxtToHtmlTest, ValidMultiBytePassesThrough) {
  EXPECT_EQ(convert("caf\xc3\xa9 \xe2\x98\x95 \xf0\x9f\x92\xa9"),
            kHeader + "caf\xc3\xa9 \xe2\x98\x95 \xf0\x9f\x92\xa9" + kFooter);
  // Later continuations are plain 0x80-0xBF even when the first one sits in a
  // restricted window (U+0905 = E0 A4 85, U+1F600 = F0 9F 98 80).
  EXPECT_EQ(convert("\xe0\xa4\x85 \xf0\x9f\x98\x80"), kHeader + "\xe0\xa4\x85 \xf0\x9f\x98\x80" + kFooter);
}

TEST(TxtToHtmlTest, ReplacesInvalidUtf8WithReplacementChar) {
  const std::string body = convert(
      "a\xff"
      "b\xc0\xaf"
      "c\xe0\x80\x80"
      "d\xed\xa0\x80"
      "e\xf5\x90\x80\x80"
      "f\x80");
  const std::string replacement = "\xef\xbf\xbd";
  EXPECT_NE(body.find(replacement), std::string::npos);
  EXPECT_EQ(body.find('\xff'), std::string::npos);
  EXPECT_EQ(body.find('\xc0'), std::string::npos);
  EXPECT_EQ(body.find("\xed\xa0\x80"), std::string::npos);
  EXPECT_EQ(body.find('\xf5'), std::string::npos);
  EXPECT_EQ(body.find('\x80'), std::string::npos);
}

TEST(TxtToHtmlTest, TruncatedMultiByteAtEndBecomesReplacement) {
  const std::string body = convert("end\xe2\x82");
  EXPECT_NE(body.find("\xef\xbf\xbd"), std::string::npos);
  EXPECT_EQ(body.find("\xe2\x82\n"), std::string::npos);
}

TEST(TxtToHtmlTest, MultiByteAcrossReadChunkBoundary) {
  // The lead byte lands in the last byte of the first 8192-byte read chunk;
  // its continuation opens the next chunk.
  std::string content(8191, 'a');
  content += "\xc3\xa9";
  EXPECT_NE(convert(content).find("\xc3\xa9"), std::string::npos);
}

TEST(TxtToHtmlTest, EarlyStopTreatsShortWriteAsSuccess) {
  class TruncatingPrint : public StringPrint {
   public:
    size_t cap;
    explicit TruncatingPrint(size_t c) : cap(c) {}
    size_t write(const uint8_t* buffer, size_t size) override {
      const size_t n = std::min(size, cap);
      str.append(reinterpret_cast<const char*>(buffer), n);
      cap -= n;
      return n;
    }
  };

  TruncatingPrint early(40);
  EXPECT_TRUE(TxtToHtml::stream("test.txt", "hello world", early, /*allowEarlyStop=*/true));
  EXPECT_EQ(early.str.size(), 40);

  TruncatingPrint strict(40);
  EXPECT_FALSE(TxtToHtml::stream("test.txt", "hello world", strict, /*allowEarlyStop=*/false));
}

}  // namespace
