#include <gtest/gtest.h>

#include <string>

#include "src/activities/reader/DictHtmlSniff.h"

using dict_html::kMaxStyledHtmlBytes;
using dict_html::looksHtml;
using dict_html::routesToStyled;

namespace {

std::string oversizeHtml() {
  std::string def = "<k>reversible</k>";
  def.resize(kMaxStyledHtmlBytes + 1, 'x');
  return def;
}

}  // namespace

// M-WAWLD ships HTML entries while its .ifo declares sametypesequence=x: the
// content sniff must route them through the styled layout even though the
// declared format is plain.
TEST(DictionaryDefinitionRouting, HeadwordTagSniffsHtmlDespiteIfoPlain) {
  const std::string def = "<k>reversible</k>\n<blockquote>capable of being reversed</blockquote>";
  EXPECT_TRUE(looksHtml(def));
  EXPECT_TRUE(routesToStyled(def, /*ifoHtml=*/false));
}

TEST(DictionaryDefinitionRouting, PlainTextStaysPlain) {
  const std::string def = "some definition text";
  EXPECT_FALSE(looksHtml(def));
  EXPECT_FALSE(routesToStyled(def, /*ifoHtml=*/false));
}

TEST(DictionaryDefinitionRouting, ShortOrMalformedIsPlain) {
  EXPECT_FALSE(looksHtml(""));
  EXPECT_FALSE(looksHtml("<a"));
  EXPECT_FALSE(looksHtml("<a>"));  // under the 4-byte minimum
  EXPECT_FALSE(looksHtml("plain with a > sign only"));
  // Leading tag opener whose first '>' sits past the sniff window is not a
  // definition header.
  EXPECT_FALSE(looksHtml("<" + std::string(70, 'a') + ">tail"));
  // '>' exactly at the window edge (byte 64) still counts.
  EXPECT_TRUE(looksHtml("<" + std::string(63, 'a') + ">tail"));
}

TEST(DictionaryDefinitionRouting, SizeGateWinsOverContentSniff) {
  const std::string def = oversizeHtml();
  EXPECT_TRUE(looksHtml(def));                           // content is HTML-shaped...
  EXPECT_FALSE(routesToStyled(def, /*ifoHtml=*/false));  // ...but the ceiling routes plain
  EXPECT_FALSE(routesToStyled(def, /*ifoHtml=*/true));   // ceiling applies to declared HTML too
}

TEST(DictionaryDefinitionRouting, CeilingBoundaryIsInclusive) {
  EXPECT_TRUE(routesToStyled(std::string(kMaxStyledHtmlBytes, 'a'), /*ifoHtml=*/true));
  EXPECT_FALSE(routesToStyled(std::string(kMaxStyledHtmlBytes + 1, 'a'), /*ifoHtml=*/true));
}

TEST(DictionaryDefinitionRouting, DeclaredHtmlRoutesStyled) {
  EXPECT_TRUE(routesToStyled("plain text", /*ifoHtml=*/true));
  EXPECT_TRUE(routesToStyled("<k>w</k>", /*ifoHtml=*/true));
}
