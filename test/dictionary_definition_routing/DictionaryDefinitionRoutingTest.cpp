#include <gtest/gtest.h>

#include <string>

#include "src/activities/reader/DictHtmlSniff.h"
#include "src/util/DictIfoTypes.h"

using dict_html::kMaxStyledHtmlBytes;
using dict_html::looksHtml;
using dict_html::routesToStyled;
using dict_ifo::declaresMarkupType;

namespace {

// StarDict types the spec declares as markup.
const char* const kMarkupTypes[] = {"h", "x", "g", "k", "y", "r", "w"};

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

// StarDict types the spec renders as markup route to the styled layout when
// the .ifo declares them, whatever the entry content looks like; multi-type
// and 'm'/unlisted sequences keep the plain-text path, leaving the sniff as
// the only way tag-soup entries from such dictionaries reach styled layout.
TEST(DictionaryDefinitionRouting, DeclaredMarkupTypesRouteStyled) {
  const std::string tagSoup = "<k>reversible</k>\n<blockquote>capable of being reversed</blockquote>";
  for (const char* type : kMarkupTypes) {
    ASSERT_TRUE(declaresMarkupType(type)) << "type " << type;
    EXPECT_TRUE(routesToStyled(tagSoup, /*ifoHtml=*/true)) << "type " << type;
    EXPECT_TRUE(routesToStyled("plain definition text", /*ifoHtml=*/true)) << "type " << type;
  }
}

TEST(DictionaryDefinitionRouting, PlainUnlistedAndMultiTypeSequencesStayPlain) {
  EXPECT_FALSE(declaresMarkupType("m"));
  EXPECT_FALSE(declaresMarkupType("l"));  // unlisted
  EXPECT_FALSE(declaresMarkupType(""));
  EXPECT_FALSE(declaresMarkupType(nullptr));
  // Multi-type sequences keep the plain-text path: per-type field semantics
  // are not honored by the single plain/styled split.
  EXPECT_FALSE(declaresMarkupType("hg"));
  EXPECT_FALSE(declaresMarkupType("xm"));
  EXPECT_FALSE(declaresMarkupType("hxg"));
}

// With the declared format left plain (m/none/multi-type all classify the
// same), the content sniff is what still routes tag-soup entries to the
// styled layout, and plain text stays plain.
TEST(DictionaryDefinitionRouting, PlainDeclaredSequencesLeanOnContentSniff) {
  const std::string tagSoup = "<k>reversible</k>\n<blockquote>capable of being reversed</blockquote>";
  for (const char* seq : {"m", "hg", "xm", "", "l"}) {
    EXPECT_FALSE(declaresMarkupType(seq)) << "seq " << seq;
    EXPECT_TRUE(routesToStyled(tagSoup, /*ifoHtml=*/false)) << "seq " << seq;
    EXPECT_FALSE(routesToStyled("plain definition text", /*ifoHtml=*/false)) << "seq " << seq;
  }
}

TEST(DictionaryDefinitionRouting, DeclaredHtmlRoutesStyled) {
  EXPECT_TRUE(routesToStyled("plain text", /*ifoHtml=*/true));
  EXPECT_TRUE(routesToStyled("<k>w</k>", /*ifoHtml=*/true));
}
