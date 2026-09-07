#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "src/util/DictionarySelection.h"

TEST(DictionarySelection, RejectsStandalonePunctuation) {
  EXPECT_FALSE(DictionarySelection::isSelectableToken("..."));
  EXPECT_FALSE(DictionarySelection::isSelectableToken("—"));
  EXPECT_TRUE(DictionarySelection::isSelectableToken("word"));
  EXPECT_TRUE(DictionarySelection::isSelectableToken("中文"));
  EXPECT_TRUE(DictionarySelection::isSelectableToken("العربية"));
}

TEST(DictionarySelection, TrimsOnlyOuterPunctuation) {
  const std::string token = "“well-being,”";
  const auto span = DictionarySelection::trimTokenEdges(token);
  EXPECT_EQ(span.start, 3U);
  EXPECT_EQ(span.length, 10U);
  EXPECT_EQ(token.substr(span.start, span.length), "well-being");
}

TEST(DictionarySelection, TrimsUtf8PunctuationAndSoftHyphen) {
  const std::string token =
      "\xE2\x80\x9C"
      "\xC3\xA9"
      "clair"
      "\xC2\xAD"
      "\xE2\x80\x9D";
  const auto span = DictionarySelection::trimTokenEdges(token);
  EXPECT_EQ(token.substr(span.start, span.length),
            "\xC3\xA9"
            "clair");
}

TEST(DictionarySelection, PreservesExplicitInternalHyphensAndApostrophes) {
  const std::string token = "rock'n\xE2\x80\x99roll";
  const auto span = DictionarySelection::trimTokenEdges(token);
  EXPECT_EQ(token.substr(span.start, span.length), token);
}

TEST(DictionarySelection, RemovesOnlySyntheticLineBreakHyphen) {
  EXPECT_EQ(DictionarySelection::logicalSegmentLength("cross-", true), 5U);
  EXPECT_EQ(DictionarySelection::logicalSegmentLength("cross-", false), 6U);
  EXPECT_EQ(DictionarySelection::logicalSegmentLength("cross", true), 5U);
}

TEST(DictionarySelection, GroupsWrappedSegmentsInFirstSeenOrder) {
  const auto grouped = DictionarySelection::groupTokens({42, 42, 77, 42, 99});

  ASSERT_EQ(grouped.groups.size(), 3U);
  EXPECT_EQ(grouped.groups[0].sourceGroup, 42U);
  EXPECT_EQ(grouped.groups[0].segmentStart, 0U);
  EXPECT_EQ(grouped.groups[0].segmentCount, 3U);
  EXPECT_EQ(grouped.groups[1].sourceGroup, 77U);
  EXPECT_EQ(grouped.groups[1].segmentStart, 3U);
  EXPECT_EQ(grouped.groups[1].segmentCount, 1U);
  EXPECT_EQ(grouped.groups[2].sourceGroup, 99U);
  EXPECT_EQ(grouped.segments, (std::vector<size_t>{0, 1, 3, 2, 4}));
}

TEST(DictionarySelection, HandlesEmptyAndInvalidUtf8WithoutStalling) {
  EXPECT_FALSE(DictionarySelection::isSelectableToken(""));
  const std::string invalid("\xE2x", 2);
  EXPECT_TRUE(DictionarySelection::isSelectableToken(invalid));
  const auto span = DictionarySelection::trimTokenEdges(invalid);
  EXPECT_LE(span.start + span.length, invalid.size());
}
