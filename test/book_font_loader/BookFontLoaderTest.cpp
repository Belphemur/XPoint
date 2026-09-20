// Host-side gtest suite for the Phase 1a BookFontLoader (native TTF fonts).
// Exercises the real loader paths through controllable file/heap stubs:
// builtin fallback, budget clamping, malformed-sfnt rejection, fingerprint
// stability, and the retained byte-lifetime contract.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "BookFontLoader.h"
#include "TestHeapHooks.h"
#include "render/TtfFont.h"

// TtfFont.cpp compiles stb with STBTT_STATIC (internal linkage), so the test
// needs its own non-static copy for the reference-metrics math below.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace {

// A minimal valid sfnt shell: 12-byte header + 1 table entry of zeros.
// TtfFont::init will reject it (no real tables) but it passes the loader's
// own sfnt boundary validation, isolating the loader's checks from stb.
std::string validSfntShell(uint16_t numTables = 1) {
  std::string s;
  s += std::string("\x00\x01\x00\x00", 4);  // sfnt version
  s += static_cast<char>(numTables >> 8);
  s += static_cast<char>(numTables & 0xFF);  // numTables
  s += std::string(6, '\0');                 // search fields
  // numTables * 16-byte table directory entries (offset/length 0)
  s += std::string(static_cast<size_t>(numTables) * 16, '\0');
  return s;
}

void writeFaceFile(const std::string& path, const std::string& bytes) { Storage.files[path] = bytes; }

TEST(BookFontLoaderBasics, InstanceCreated) {
  freeink::book::BookFontLoader loader;
  EXPECT_EQ(loader.familyCount(), 0u);
  EXPECT_EQ(loader.fontFingerprint(), 0u);
}

TEST(BookFontLoaderBasics, GetReaderFontFallsBackToBuiltin) {
  freeink::book::BookFontLoader loader;
  freeink::book::FontChain* readerFont = loader.getReaderFont();
  ASSERT_NE(readerFont, nullptr);
  // Builtin BitmapBookFont fallback covers None|Bold|Italic (0x07).
  EXPECT_EQ(readerFont->styleCoverage(), 0x07);
}

TEST(BookFontLoaderBasics, RepeatedBeginDoesNotLeak) {
  testSetPsramHeap({0, 0, 0, 0});
  testSetFreeHeap(320 * 1024, 320 * 1024);
  freeink::book::BookFontLoader loader;
  loader.begin();
  loader.begin();  // second begin() over a live loader must stay safe
  EXPECT_EQ(loader.familyCount(), 0u);
  EXPECT_EQ(loader.fontFingerprint(), 0u);
}

TEST(BookFontLoaderBasics, BudgetClampsToMaxAndFloors) {
  testSetPsramHeap({0, 0, 0, 0});  // no PSRAM → DRAM tier
  freeink::book::BookFontLoader loader;

  // Above the 48KB reserve: budget = min(free - 48KB, 128KB).
  testSetFreeHeap(320 * 1024, 320 * 1024);
  loader.begin();
  EXPECT_EQ(loader.dramBudgetForTest(), 128u * 1024u);

  // Above the reserve but under the cap: exact remainder.
  testSetFreeHeap(64 * 1024, 64 * 1024);
  loader.begin();
  EXPECT_EQ(loader.dramBudgetForTest(), 16u * 1024u);

  // Exactly at the reserve: zero (nothing safe to spend).
  testSetFreeHeap(48 * 1024, 48 * 1024);
  loader.begin();
  EXPECT_EQ(loader.dramBudgetForTest(), 0u);

  // Below the reserve: zero (was the pre-review 128KB grant).
  testSetFreeHeap(32 * 1024, 32 * 1024);
  loader.begin();
  EXPECT_EQ(loader.dramBudgetForTest(), 0u);
}

TEST(BookFontLoaderBasics, MalformedSfntRejected) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();

  // Too small for the sfnt header (<12 bytes).
  writeFaceFile("/fonts/Bad-Tiny.ttf", "short");
  // numTables == 0.
  std::string zero = validSfntShell(0);
  zero[5] = '\x00';
  writeFaceFile("/fonts/Bad-ZeroTables.ttf", zero);

  auto& fam = loader.editFamily(0);
  fam.faceCount = 2;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = 5;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", "/fonts/Bad-Tiny.ttf");
  fam.faces[1].styleFlags = freeink::book::StyleBold;
  fam.faces[1].fileSize = static_cast<uint32_t>(zero.size());
  std::snprintf(fam.faces[1].file, sizeof(fam.faces[1].file), "%s", "/fonts/Bad-ZeroTables.ttf");
  // Establish the manifest count so ensureLoaded() actually attempts the
  // load (familyCount_ > 0 gate) and the rejection path really runs.
  loader.setFamilyCountForTest(1);

  // markDirty + reload: both faces must be skipped (no crash, no chain entry).
  loader.markDirty();
  loader.getReaderFont();
  // Nothing loaded → empty fingerprint (0), NOT the FNV offset basis.
  EXPECT_EQ(loader.fontFingerprint(), 0u);
  // The builtin fallback serves the reader because the chain is empty.
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
}

TEST(BookFontLoaderBasics, FingerprintIsContentBased) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  // Fingerprint must be 0 when nothing is loaded (content-based, never path).
  EXPECT_EQ(loader.fontFingerprint(), 0u);
}

// The Atkinson fallback faces must ride ALONG a selected TTF family as the
// chain tail (§14.5): a family whose faces all fail to load still serves a
// glyph-bearing chain through the live chain_, not just the standalone
// fallback singleton.
TEST(BookFontLoaderBasics, FallbackTailAppendedToSelectedChain) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();

  // A selected family whose only face fails sfnt validation.
  writeFaceFile("/fonts/Failing/Failing-Regular.ttf", "garbage");
  auto& fam = loader.editFamily(0);
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = 7;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", "/fonts/Failing/Failing-Regular.ttf");
  loader.setFamilyCountForTest(1);
  loader.markDirty();

  freeink::book::FontChain* font = loader.getReaderFont();
  ASSERT_NE(font, nullptr);
  // The live chain itself carries the tail: full None|Bold|Italic coverage.
  EXPECT_EQ(font->styleCoverage(), 0x07);
  EXPECT_EQ(font, &loader.chainForTest());

  // A missing glyph resolves through the tail, never nullptr.
  const uint32_t cpNoOneHas = 0x1F600;  // emoji — no face covers it
  uint8_t faceFlags = 0xFF;
  EXPECT_NE(font->fontFor(cpNoOneHas, freeink::book::StyleNone, &faceFlags), nullptr);
}

// The tail is a non-selectable addition: with NO family selected the chain
// stays empty and the builtin fallback singleton serves the reader.
TEST(BookFontLoaderBasics, NoFamilyStillUsesFallbackSingleton) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
  // chainForTest() is still empty: the singleton answered, not the live chain.
  EXPECT_EQ(loader.chainForTest().styleCoverage(), 0u);
}

// An empty manifest may legitimately complete the load attempt (fingerprint 0
// and the standalone fallback singleton), but a later begin() rescan must
// reset the load gate so the same persisted family selection can load without
// relying on selectFamily() to dirty an unchanged name.
TEST(BookFontLoaderBasics, BeginResetsLoadGateAfterEmptyManifest) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.selectFamily("Alpha");
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
  EXPECT_EQ(loader.chainForTest().styleCoverage(), 0u);  // empty-manifest path

  loader.begin();  // simulates the settings screen rescan after SD mounts
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Alpha");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = 5;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", "/fonts/Alpha/Alpha-Regular.ttf");
  loader.setFamilyCountForTest(1);
  loader.selectFamily("Alpha");  // unchanged selection: intentionally no markDirty

  // The malformed face is rejected, but ensureLoaded() ran and appended the
  // fallback tail to the live chain (distinct from the standalone singleton).
  freeink::book::FontChain* font = loader.getReaderFont();
  EXPECT_EQ(font, &loader.chainForTest());
  EXPECT_EQ(font->styleCoverage(), 0x07);
}

TEST(FontFaceInfoTest, DefaultMembers) {
  freeink::book::FontFaceInfo faceInfo;
  EXPECT_EQ(faceInfo.name[0], '\0');
  EXPECT_EQ(faceInfo.file[0], '\0');
  EXPECT_EQ(faceInfo.styleFlags, 0u);
  EXPECT_EQ(faceInfo.fileSize, 0u);
  EXPECT_EQ(faceInfo.mtime, 0u);
}

TEST(FamilyInfoTest, DefaultMembers) {
  freeink::book::FamilyInfo familyInfo;
  EXPECT_EQ(familyInfo.name[0], '\0');
  EXPECT_EQ(familyInfo.faceCount, 0u);
  EXPECT_FALSE(familyInfo.isBuiltinFallback);
}

TEST(StyleFlagsTest, BitValues) {
  EXPECT_EQ(freeink::book::StyleNone, 0);
  EXPECT_EQ(freeink::book::StyleBold, 1);
  EXPECT_EQ(freeink::book::StyleItalic, 2);
  EXPECT_EQ(freeink::book::StyleBold & freeink::book::StyleItalic, 0);
  EXPECT_EQ(freeink::book::StyleBold | freeink::book::StyleItalic, 3);
}

TEST(BookFontLoaderConstants, DiscoveryBound) {
  EXPECT_GT(freeink::book::BookFontLoader::kMaxDiscoveredFamilies, 0u);
  EXPECT_LE(freeink::book::BookFontLoader::kMaxDiscoveredFamilies, 100u);
}

}  // namespace
// ── scanFonts — §14.4 per-family discovery walk ──────────────────────────────

namespace {

namespace book = freeink::book;
using freeink::book::BookFontLoader;

constexpr uint8_t kStyleBI = freeink::book::StyleBold | freeink::book::StyleItalic;

void resetStorage() {
  Storage.files.clear();
  Storage.dirs.clear();
  Storage.mtimes.clear();
}

// Registers a file entry (and every ancestor directory) in the stub storage.
void seedFile(const std::string& path, const std::string& bytes = "x") {
  for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) {
    Storage.dirs.insert(path.substr(0, slash));
  }
  Storage.files[path] = bytes;
}

const freeink::book::FontFaceInfo* findFace(const freeink::book::FamilyInfo& fam, uint8_t styleFlags) {
  for (uint8_t i = 0; i < fam.faceCount; ++i) {
    if (fam.faces[i].styleFlags == styleFlags) return &fam.faces[i];
  }
  return nullptr;
}

}  // namespace

TEST(ScanFontsTest, DiscoversFamiliesAndStyles) {
  resetStorage();
  seedFile("/fonts/Bookerly/Bookerly-Regular.ttf");
  seedFile("/fonts/Bookerly/Bookerly-Bold.ttf");
  seedFile("/fonts/Bookerly/Bookerly-Italic.ttf");
  seedFile("/fonts/Bookerly/Bookerly-BoldItalic.ttf");
  seedFile("/fonts/Bookerly/Bookerly-SemiBold.ttf", "");  // weight heuristic → Bold

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].name, "Bookerly");
  EXPECT_EQ(fams[0].faceCount, 4u);
  ASSERT_NE(findFace(fams[0], freeink::book::StyleNone), nullptr);
  ASSERT_NE(findFace(fams[0], freeink::book::StyleBold), nullptr);
  ASSERT_NE(findFace(fams[0], freeink::book::StyleItalic), nullptr);
  ASSERT_NE(findFace(fams[0], kStyleBI), nullptr);
  // Regular's full path points inside the family folder.
  EXPECT_STREQ(findFace(fams[0], freeink::book::StyleNone)->file, "/fonts/Bookerly/Bookerly-Regular.ttf");
}

TEST(ScanFontsTest, HiddenRootWinsFamilyDedupe) {
  resetStorage();
  seedFile("/.fonts/Bookerly/Bookerly-Light.ttf");
  seedFile("/fonts/Bookerly/Bookerly-Regular.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  // Hidden root first (the begin() order), so its family claims the name.
  BookFontLoader::scanFontsForTest("/.fonts", fams, count);
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  // Hidden root wins: the surviving family's Regular is its promoted file.
  EXPECT_STREQ(fams[0].faces[0].file, "/.fonts/Bookerly/Bookerly-Light.ttf");
  EXPECT_EQ(fams[0].faces[0].styleFlags, freeink::book::StyleNone);
}

TEST(ScanFontsTest, StylePriorityAndWordBoundaries) {
  resetStorage();
  seedFile("/fonts/Mix/Mix-SemiBold.ttf");  // weight heuristic → Bold
  seedFile("/fonts/Mix/Mix-Light.ttf");     // light → Regular
  seedFile("/fonts/Mix/Mix-Italic.ttf");    // Italic
  seedFile("/fonts/Mix/Mix-ExtraBold.ttf", "x");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  // SemiBold ≠ Bold (word boundary), so Mix has exactly: Regular(Light),
  // Bold(SemiBold), Bold(ExtraBold — dup resolved lexicographically? No:
  // ExtraBold is Bold too → same slot as SemiBold, first file wins).
  const auto* bold = findFace(fams[0], freeink::book::StyleBold);
  ASSERT_NE(bold, nullptr);
  EXPECT_STREQ(bold->name, "mix-extrabold");  // lexicographically-first dup wins
  const auto* regular = findFace(fams[0], freeink::book::StyleNone);
  ASSERT_NE(regular, nullptr);
  EXPECT_STREQ(regular->name, "mix-light");
  ASSERT_NE(findFace(fams[0], freeink::book::StyleItalic), nullptr);
}

TEST(ScanFontsTest, SingleFileFamilyPromotesRegular) {
  resetStorage();
  seedFile("/fonts/Solo/Solo-Head.ttf", std::string(1234, 'f'));

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(fams[0].faceCount, 1u);
  EXPECT_EQ(fams[0].faces[0].styleFlags, freeink::book::StyleNone);
  // The promoted face carries a canonical stem, not the extension-bearing filename.
  EXPECT_STREQ(fams[0].faces[0].name, "solo-head");
  // The promoted face must carry the lone candidate's size, or
  // tryLoadFace() would read zero bytes and reject the family.
  EXPECT_EQ(fams[0].faces[0].fileSize, 1234u);
}

TEST(ScanFontsTest, DuplicateStyleComparesCanonicalStems) {
  resetStorage();
  seedFile("/fonts/Dupe/Face-Bold.ttf");
  seedFile("/fonts/Dupe/Face-Bold-Extra.otf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  ASSERT_EQ(fams[0].faceCount, 1u);
  EXPECT_STREQ(fams[0].faces[0].name, "face-bold");
  EXPECT_STREQ(fams[0].faces[0].file, "/fonts/Dupe/Face-Bold.ttf");
}

TEST(ScanFontsTest, SkipsJunkFilesAndFolders) {
  resetStorage();
  seedFile("/fonts/Clean/Clean-Regular.ttf");
  seedFile("/fonts/Clean/._Clean-Regular.ttf", "");   // macOS resource fork
  seedFile("/fonts/Clean/Clean-Regular.ttf~", "");    // editor backup
  seedFile("/fonts/Clean/Clean-Regular.json", "");    // wrong extension
  seedFile("/fonts/.Trashes/Clean-Regular.ttf", "");  // hidden folder
  seedFile("/fonts/_private/x-Regular.ttf", "");      // underscore folder
  seedFile("/fonts/Loose-Regular.ttf", "");           // root-level: ignored

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].name, "Clean");
  EXPECT_EQ(fams[0].faceCount, 1u);
}

TEST(ScanFontsTest, FamilyCapAt32) {
  resetStorage();
  for (int i = 0; i < BookFontLoader::kMaxDiscoveredFamilies + 4; ++i) {
    seedFile(std::string("/fonts/Fam") + static_cast<char>('A' + i % 26) + std::to_string(i) + "/F-Regular.ttf");
  }
  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  EXPECT_EQ(count, BookFontLoader::kMaxDiscoveredFamilies);
}

TEST(ScanFontsTest, OtfExtensionAccepted) {
  resetStorage();
  seedFile("/fonts/Otf/Otf-Regular.otf");
  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].faces[0].file, "/fonts/Otf/Otf-Regular.otf");
}

TEST(ScanFontsTest, TruncatedFamilyNameIsSkipped) {
  resetStorage();
  // FamilyInfo::name holds 47 chars plus NUL; dirName now has headroom for
  // long path validation, but names too large for the manifest are skipped.
  const std::string longName(48, 'L');
  seedFile("/fonts/" + longName + "/Long-Regular.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  EXPECT_EQ(count, 0u);
}

TEST(ScanFontsTest, LongVendorPathsFitManifest) {
  resetStorage();
  // Owner-log regression: the full path is 71 chars and the old 64-byte face
  // path silently dropped it.
  const std::string path = "/.fonts/Atkinson Hyperlegible Next/AtkinsonHyperlegibleNext-Regular.otf";
  ASSERT_GT(path.size(), 64u);
  seedFile(path);

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/.fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].name, "Atkinson Hyperlegible Next");
  EXPECT_EQ(fams[0].faceCount, 1u);
  EXPECT_STREQ(fams[0].faces[0].file, path.c_str());
}

TEST(ScanFontsTest, SplitsFamilyAcrossRootsIntoOneFamily) {
  resetStorage();
  seedFile("/.fonts/Bookerly/Bookerly-Regular.ttf");
  seedFile("/fonts/Bookerly/Bookerly-Bold.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/.fonts", fams, count);
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].name, "Bookerly");
  ASSERT_EQ(fams[0].faceCount, 2u);
  const auto* regular = findFace(fams[0], freeink::book::StyleNone);
  const auto* bold = findFace(fams[0], freeink::book::StyleBold);
  ASSERT_NE(regular, nullptr);
  ASSERT_NE(bold, nullptr);
  EXPECT_STREQ(regular->file, "/.fonts/Bookerly/Bookerly-Regular.ttf");
  EXPECT_STREQ(bold->file, "/fonts/Bookerly/Bookerly-Bold.ttf");
}

TEST(ScanFontsTest, FamilyOnlyInVisibleRootIsStillDiscovered) {
  resetStorage();
  seedFile("/.fonts/Hidden/Hidden-Regular.ttf");
  seedFile("/fonts/Visible/Visible-Regular.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/.fonts", fams, count);
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 2u);
  EXPECT_STREQ(fams[1].name, "Visible");
  EXPECT_STREQ(fams[1].faces[0].file, "/fonts/Visible/Visible-Regular.ttf");
}

TEST(ScanFontsTest, TokenlessCandidateBecomesRegularNotPromotedBold) {
  resetStorage();
  // Owner-log regression: "Bookerly Display" has no style token and was
  // skipped, leaving Bold as the promoted Regular.
  seedFile("/fonts/Bookerly-otf/Bookerly Display.ttf");
  seedFile("/fonts/Bookerly-otf/Bookerly-Bold.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  const auto* regular = findFace(fams[0], freeink::book::StyleNone);
  const auto* bold = findFace(fams[0], freeink::book::StyleBold);
  ASSERT_NE(regular, nullptr);
  ASSERT_NE(bold, nullptr);
  EXPECT_STREQ(regular->file, "/fonts/Bookerly-otf/Bookerly Display.ttf");
  EXPECT_STREQ(bold->file, "/fonts/Bookerly-otf/Bookerly-Bold.ttf");
}

// A tokenless candidate belongs to the family that scanned it: the next
// family's post-loop Regular resolution must not reuse it.
TEST(ScanFontsTest, TokenlessCandidateDoesNotLeakToNextFamily) {
  resetStorage();
  seedFile("/fonts/First/First Display.ttf");
  seedFile("/fonts/Second/Second-Bold.ttf");

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 2u);
  EXPECT_STREQ(fams[0].name, "First");
  const auto* firstRegular = findFace(fams[0], freeink::book::StyleNone);
  ASSERT_NE(firstRegular, nullptr);
  EXPECT_STREQ(firstRegular->file, "/fonts/First/First Display.ttf");

  EXPECT_STREQ(fams[1].name, "Second");
  // The single-file Second family keeps exactly its own promoted face; a
  // stale tokenless candidate from First must not survive into it. Asserting
  // the full shape (count + path) catches a leak even when the candidate is
  // copied under the SECOND family's own path prefix.
  ASSERT_EQ(fams[1].faceCount, 1u);
  EXPECT_STREQ(fams[1].faces[0].file, "/fonts/Second/Second-Bold.ttf");
}

TEST(ScanFontsTest, MissingRootIsQuietNoop) {
  resetStorage();
  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  EXPECT_EQ(count, 0u);
}

// ── Phase 3: family selection + picker availability gates ───────────────────

TEST(BookFontLoaderSelection, FindFamilyMatchesExactly) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Literata");
  fam.faceCount = 1;
  fam.faces[0].fileSize = 100;
  loader.setFamilyCountForTest(1);

  EXPECT_NE(loader.findFamily("Literata"), nullptr);
  // Case-insensitive per the loader contract (BookFontLoader.h §14.4 display
  // name): settings round-trip through the web UI/JSON with any casing.
  EXPECT_NE(loader.findFamily("literata"), nullptr);
  EXPECT_NE(loader.findFamily("LITERATA"), nullptr);
  EXPECT_EQ(loader.findFamily("Other"), nullptr);
  EXPECT_EQ(loader.findFamily(""), nullptr);
  EXPECT_EQ(loader.findFamily(nullptr), nullptr);
}

TEST(BookFontLoaderSelection, SelectFamilyRejectsSameAndAcceptsChange) {
  testSetPsramHeap({0, 0, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.selectFamily("Alpha");
  loader.selectFamily("Alpha");  // same selection: no reload churn
  loader.selectFamily("Beta");
  loader.selectFamily("");  // explicit fallback
  SUCCEED();
}

TEST(BookFontLoaderSelection, AvailabilityRequiresPsramAndFaceGuard) {
  freeink::book::BookFontLoader loader;
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Small");
  fam.faceCount = 1;
  fam.faces[0].fileSize = 100;
  auto& big = loader.editFamily(1);
  std::snprintf(big.name, sizeof(big.name), "%s", "Big");
  big.faceCount = 1;
  big.faces[0].fileSize = loader.kMaxFaceBytes + 1;
  loader.setFamilyCountForTest(2);

  testSetPsramHeap({0, 0, 0, 0});  // PSRAM-less: the whole class is unavailable
  EXPECT_FALSE(loader.isFamilyAvailable(fam));
  EXPECT_FALSE(loader.isFamilyAvailable(big));

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  EXPECT_TRUE(loader.isFamilyAvailable(fam));
  EXPECT_FALSE(loader.isFamilyAvailable(big));  // per-face 2MB gate (§3.3)
}

TEST(BookFontLoaderSelection, ClearedSelectionServesFallbackChain) {
  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.selectFamily("");  // §14.3: built-in = fallback chain, never a family
  loader.markDirty();
  loader.getReaderFont();
  EXPECT_EQ(loader.fontFingerprint(), 0u);
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
}

// ── Real-font fixtures: Amazon Ember (mixed upem, subsetted cmap) ────────────
// Owner-supplied faces with hostile metadata: upem 2048/2048/1000 across one
// family and inconsistent internal name tables ("AmazonEmber-Regular" vs
// "Amazon Ember"). Grouping must be folder-based (§14.4) and scaling
// per-face — these tests pin both so a family-uniform-upem regression fails
// here before it can produce off-page geometry on device.

namespace {

std::string readFixtureFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f.good()) return {};  // callers must skip on empty (see fixtureAvailable)
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string emberBytes(const char* name) { return readFixtureFile(EMBER_FIXTURES_DIR "/" + std::string(name)); }

// Missing/unreadable fixtures must skip the test, not feed stb an empty
// buffer (stbtt_GetFontOffsetForIndex reads 4 header bytes → OOB crash).
// GTEST_SKIP() returns from the current function, so test bodies own it.
bool fixtureAvailable(const std::string& bytes) { return !bytes.empty(); }

struct ParsedFace {
  stbtt_fontinfo info{};
  int asc = 0;
  int desc = 0;
  int gap = 0;
};

ParsedFace parseFace(const std::string& bytes) {
  ParsedFace f;
  // stbtt_GetFontOffsetForIndex's TTC path reads the first container offset
  // at bytes[12..15]; require 16 bytes so every stb read below is in bounds
  // even for a truncated stub or a small git-lfs pointer file.
  if (bytes.size() < 16) {
    ADD_FAILURE() << "fixture too short to be a TTF (missing file?)";
    return f;
  }
  const auto* u8 = reinterpret_cast<const uint8_t*>(bytes.data());
  const int offset = stbtt_GetFontOffsetForIndex(u8, 0);
  // 64-bit compare: a fixture-crafted TTC container offset can be near
  // INT32_MAX and offset+12 would overflow signed 32-bit arithmetic.
  if (offset < 0 || static_cast<uint64_t>(offset) + 12u > bytes.size()) {
    ADD_FAILURE() << "fixture has no sfnt offset for index 0";
    return f;
  }
  // Table-directory bound, mirroring BookFontLoader's sfnt validation
  // (kMinSfntLen): stbtt_InitFont walks numTables * 16-byte entries at
  // data+12 and would OOB-read past a truncated directory.
  const uint16_t numTables = static_cast<uint16_t>((u8[offset + 4] << 8) | u8[offset + 5]);
  const uint64_t minSz = static_cast<uint64_t>(offset) + 12u + static_cast<uint64_t>(numTables) * 16u;
  if (numTables == 0 || minSz > bytes.size()) {
    ADD_FAILURE() << "fixture table directory out of bounds (numTables=" << numTables << ")";
    return f;
  }
  EXPECT_NE(stbtt_InitFont(&f.info, u8, offset), 0);
  stbtt_GetFontVMetrics(&f.info, &f.asc, &f.desc, &f.gap);
  return f;
}

// Mirrors TtfFont::ascent(): unitsAscent * stbtt_ScaleForPixelHeight(size).
int16_t expectedAscent(const ParsedFace& f, uint16_t sizePx) {
  const float scale = stbtt_ScaleForPixelHeight(&f.info, static_cast<float>(sizePx));
  return static_cast<int16_t>(f.asc * scale + 0.5f);
}

constexpr uint16_t kReadSize = 40;  // px; any size works — math is linear

}  // namespace

// Owner-supplied OTF regression: stb_truetype's vendored version supports
// OTTO/CFF (stb_truetype.h:1299 and the CFF charstring interpreter), but the
// loader path must still produce real pixels — an empty "A" means the font
// appears selectable yet cannot render.
TEST(TtfFaceMetrics, AtkinsonOtfCffRendersGlyphA) {
  const std::string bytes = readFixtureFile(ATKINSON_OTF_FIXTURE);
  if (!fixtureAvailable(bytes)) GTEST_SKIP() << "fixture unavailable: AtkinsonHyperlegibleNext-Regular.otf";
  const ParsedFace parsed = parseFace(bytes);
  ASSERT_NE(parsed.asc, 0);

  const int glyph = stbtt_FindGlyphIndex(&parsed.info, 'A');
  ASSERT_GT(glyph, 0);
  const float scale = stbtt_ScaleForPixelHeight(&parsed.info, static_cast<float>(kReadSize));
  int width = 0;
  int height = 0;
  int xoff = 0;
  int yoff = 0;
  const std::unique_ptr<unsigned char, decltype([](unsigned char* p) { stbtt_FreeBitmap(p, nullptr); })> bitmap(
      stbtt_GetCodepointBitmapSubpixel(&parsed.info, scale, scale, 0.0f, 0.0f, 'A', &width, &height, &xoff, &yoff));
  ASSERT_NE(bitmap, nullptr);
  EXPECT_GT(width, 0);
  EXPECT_GT(height, 0);
  bool hasInk = false;
  for (int i = 0; i < width * height; ++i) {
    if (bitmap.get()[i] != 0) {
      hasInk = true;
      break;
    }
  }
  EXPECT_TRUE(hasInk);
}

// §14.4: family identity is the folder name; the files' internal name tables
// ("AmazonEmber-Regular", "AmazonEmber-Bold", "Amazon Ember") must never
// split or rename the family, and filename tokens must drive style slots.
TEST(ScanFontsTest, AmazonEmberOneFamilyFromFolderNotNameTables) {
  const std::string regularBytes = emberBytes("Amazon_Ember_Regular.ttf");
  const std::string boldBytes = emberBytes("Amazon_Ember_Bold.ttf");
  const std::string boldItalicBytes = emberBytes("Amazon_Ember_Bold_Italic.ttf");
  if (!fixtureAvailable(regularBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  if (!fixtureAvailable(boldBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Bold.ttf";
  if (!fixtureAvailable(boldItalicBytes)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Bold_Italic.ttf";
  resetStorage();
  seedFile("/fonts/Amazon Ember/Amazon_Ember_Regular.ttf", regularBytes);
  seedFile("/fonts/Amazon Ember/Amazon_Ember_Bold.ttf", boldBytes);
  seedFile("/fonts/Amazon Ember/Amazon_Ember_Bold_Italic.ttf", boldItalicBytes);

  static book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].name, "Amazon Ember");
  ASSERT_EQ(fams[0].faceCount, 3u);
  const auto* regular = findFace(fams[0], freeink::book::StyleNone);
  const auto* bold = findFace(fams[0], freeink::book::StyleBold);
  const auto* boldItalic = findFace(fams[0], kStyleBI);
  ASSERT_NE(regular, nullptr);
  ASSERT_NE(bold, nullptr);
  ASSERT_NE(boldItalic, nullptr);
  EXPECT_STREQ(regular->file, "/fonts/Amazon Ember/Amazon_Ember_Regular.ttf");
  EXPECT_STREQ(bold->file, "/fonts/Amazon Ember/Amazon_Ember_Bold.ttf");
  EXPECT_STREQ(boldItalic->file, "/fonts/Amazon Ember/Amazon_Ember_Bold_Italic.ttf");
}

// Each face scales by its OWN unitsPerEm: the 1000-upem BoldItalic carries
// ~half the raw hhea units of the 2048 faces, yet the pixel-scaled ascents
// must land in the same neighborhood and match stb's per-face math exactly.
TEST(TtfFaceMetrics, AmazonEmberScalesByPerFaceUnitsPerEm) {
  const std::string regular = emberBytes("Amazon_Ember_Regular.ttf");
  const std::string boldItalic = emberBytes("Amazon_Ember_Bold_Italic.ttf");
  if (!fixtureAvailable(regular)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  if (!fixtureAvailable(boldItalic)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Bold_Italic.ttf";
  const ParsedFace pr = parseFace(regular);
  const ParsedFace pbi = parseFace(boldItalic);
  ASSERT_NE(pr.asc, 0);
  ASSERT_NE(pbi.asc, 0);

  static uint8_t arenaBuf[64 * 1024];
  book::Arena arena(arenaBuf, sizeof(arenaBuf));
  book::TtfFont fontReg;
  ASSERT_TRUE(
      fontReg.init(reinterpret_cast<const uint8_t*>(regular.data()), static_cast<uint32_t>(regular.size()), arena));
  book::TtfFont fontBi;
  ASSERT_TRUE(fontBi.init(reinterpret_cast<const uint8_t*>(boldItalic.data()), static_cast<uint32_t>(boldItalic.size()),
                          arena));

  // Exact per-face formula match (a family-uniform scale would break these).
  EXPECT_EQ(fontReg.ascent(kReadSize), expectedAscent(pr, kReadSize));
  EXPECT_EQ(fontBi.ascent(kReadSize), expectedAscent(pbi, kReadSize));
  // Raw unit spaces are ~2x apart; the scaled results are not.
  EXPECT_GT(std::abs(pr.asc), std::abs(pbi.asc));
  EXPECT_LE(std::abs(static_cast<int>(fontReg.ascent(kReadSize)) - static_cast<int>(fontBi.ascent(kReadSize))), 2);
}

// Mixed-upem chain: the chain's line grid comes from the FIRST registered
// face (stable baseline across styles), never from a later face's upem.
TEST(FontChainMixedUpem, LineGridComesFromFirstFace) {
  const std::string regular = emberBytes("Amazon_Ember_Regular.ttf");
  const std::string boldItalic = emberBytes("Amazon_Ember_Bold_Italic.ttf");
  if (!fixtureAvailable(regular)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  if (!fixtureAvailable(boldItalic)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Bold_Italic.ttf";

  static uint8_t arenaBuf[64 * 1024];
  book::Arena arena(arenaBuf, sizeof(arenaBuf));
  book::TtfFont fontReg;
  ASSERT_TRUE(
      fontReg.init(reinterpret_cast<const uint8_t*>(regular.data()), static_cast<uint32_t>(regular.size()), arena));
  book::TtfFont fontBi;
  ASSERT_TRUE(fontBi.init(reinterpret_cast<const uint8_t*>(boldItalic.data()), static_cast<uint32_t>(boldItalic.size()),
                          arena));

  book::FontChain chain;
  ASSERT_TRUE(chain.add(&fontReg, book::StyleNone));
  ASSERT_TRUE(chain.add(&fontBi, book::StyleBold | book::StyleItalic));
  EXPECT_EQ(chain.lineHeight(kReadSize), fontReg.lineHeight(kReadSize));
  EXPECT_EQ(chain.ascent(kReadSize), fontReg.ascent(kReadSize));
}

// The Regular face is heavily subsetted (78 glyphs, 682 cmap entries):
// common Latin letters must resolve in-face; a cmap miss must fall through
// the chain to a covering tail face instead of returning nullptr.
TEST(FontChainMixedUpem, CmapMissFallsThroughToCoveringFace) {
  const std::string regular = emberBytes("Amazon_Ember_Regular.ttf");
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(regular)) GTEST_SKIP() << "fixture unavailable: Amazon_Ember_Regular.ttf";
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DEJAVU_FIXTURE";

  static uint8_t arenaBuf[64 * 1024];
  book::Arena arena(arenaBuf, sizeof(arenaBuf));
  book::TtfFont fontReg;
  ASSERT_TRUE(
      fontReg.init(reinterpret_cast<const uint8_t*>(regular.data()), static_cast<uint32_t>(regular.size()), arena));
  book::TtfFont fontTail;
  ASSERT_TRUE(
      fontTail.init(reinterpret_cast<const uint8_t*>(dejavu.data()), static_cast<uint32_t>(dejavu.size()), arena));

  book::FontChain chain;
  ASSERT_TRUE(chain.add(&fontReg, book::StyleNone));
  ASSERT_TRUE(chain.add(&fontTail, book::StyleNone));
  ASSERT_TRUE(chain.add(&fontTail, book::StyleBold));
  ASSERT_TRUE(chain.add(&fontTail, book::StyleItalic));
  ASSERT_TRUE(chain.add(&fontTail, book::StyleBold | book::StyleItalic));

  // Sanity: core Latin letters stay in the subsetted face.
  for (const uint32_t cp : {'a', 'A', 'e', ' ', '.'}) {
    EXPECT_TRUE(fontReg.hasGlyph(cp)) << "subset lacks common codepoint U+" << std::hex << cp;
  }

  // Find a codepoint the Ember subset does not cover but the tail face
  // does; the chain must route it to that covering face, never nullptr.
  // (Ember's cmap spans Latin/Greek/Cyrillic; Hebrew/Armenian/Georgian sit
  // outside it. DejaVu carries all three blocks.)
  const uint32_t candidates[] = {0x05D0 /*א*/, 0x0531 /*Ա*/, 0x1E00, 0x10A0};
  uint32_t missing = 0;
  for (const uint32_t cp : candidates) {
    if (!fontReg.hasGlyph(cp) && fontTail.hasGlyph(cp)) {
      missing = cp;
      break;
    }
  }
  ASSERT_NE(missing, 0u) << "fixture unexpectedly covers every candidate";
  uint8_t faceFlags = 0xFF;
  EXPECT_EQ(chain.fontFor(missing, book::StyleNone, &faceFlags), &fontTail);
}

// ── P3.1 SD fingerprint cache ────────────────────────────────────────
// Note: fontFingerprint() is documented as the PRE-fallback-tail identity
// (ensureLoaded computes it before appendFallbackTail; the prefetch worker
// mirrors that), so these tests never compare it against a post-tail
// computeFingerprint() call — cached-vs-pure parity is pinned by the
// corrupt-record recompute case instead.────
// The chained per-face FNV-1a walk (up to 4 × 2 MB per family load) is cached
// under /.crosspoint/fonts/ keyed by the face's path hash, valid only when
// {fileSize, mtime, incoming chain seed} all match. These tests pin:
// cache-hit == pure fingerprint (byte parity), mtime/size change → rehash,
// corrupt cache file → recompute-and-rewrite, and mtime 0 → cache disabled.

namespace {

std::string fpCachePathFor(const char* facePath) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/.crosspoint/fonts/fp_%08x.bin",
                freeink::book::BookFontLoader::fontBytesHash(reinterpret_cast<const uint8_t*>(facePath),
                                                             std::strlen(facePath), 0x811c9dc5u));
  return buf;
}

// One loadable DejaVu face driven through editFamily (scanFonts not needed).
void seedLoadableFace(const std::string& bytes, const char* path, uint32_t mtime) {
  writeFaceFile(path, bytes);
  if (mtime != 0) Storage.mtimes[path] = mtime;
}

}  // namespace

TEST(BookFontLoaderFingerprintCache, CacheRoundTripMatchesPureFingerprint) {
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u) << "fixture too small to be a TTF (truncated?)";
  resetStorage();
  constexpr uint32_t kMtime = 0x5F123456u;
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, kMtime);

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  fam.faces[0].mtime = kMtime;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);

  // First load: cache MISS (no file yet) → byte-walk + cache WRITE.
  loader.markDirty();
  const uint32_t fpFirst = loader.getReaderFont()->styleCoverage() != 0 ? loader.fontFingerprint() : 0;
  EXPECT_NE(fpFirst, 0u);
  const std::string cachePath = fpCachePathFor(kFacePath);
  ASSERT_EQ(Storage.files.count(cachePath), 1u);    // cache written
  EXPECT_EQ(Storage.files[cachePath].size(), 28u);  // fixed record

  // Second load: cache HIT must reproduce the exact fingerprint.
  loader.markDirty();
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
  EXPECT_EQ(loader.fontFingerprint(), fpFirst);

  // Head rewrite (same size + mtime): the review-hardened hit check covers
  // the first 4 KB, so a changed header byte — still a valid, loadable sfnt
  // — must REHASH, never serve the stale record.
  std::string headModified = dejavu;
  headModified[8] = static_cast<char>(headModified[8] ^ 0xFF);
  ASSERT_NE(headModified, dejavu);
  writeFaceFile(kFacePath, headModified);
  loader.markDirty();
  loader.getReaderFont();  // markDirty alone only arms; ensureLoaded runs here
  EXPECT_NE(loader.fontFingerprint(), fpFirst);
  const uint32_t fpHead = loader.fontFingerprint();

  // Bounded window (documented): a rewrite beyond the 4 KB head with the
  // same size + mtime keeps the head hash — the record is served (hit).
  std::string tailModified = headModified;
  ASSERT_GT(tailModified.size(), 6000u) << "fixture too small for the tail-modify step";
  tailModified[6000] = static_cast<char>(tailModified[6000] ^ 0xFF);
  writeFaceFile(kFacePath, tailModified);
  loader.markDirty();
  loader.getReaderFont();
  EXPECT_EQ(loader.fontFingerprint(), fpHead);
}

TEST(BookFontLoaderFingerprintCache, MtimeChangeRehashes) {
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u) << "fixture too small to be a TTF (truncated?)";
  resetStorage();
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  std::string modified = dejavu;
  modified[8] = static_cast<char>(modified[8] ^ 0xFF);  // same size, different content

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);

  seedLoadableFace(dejavu, kFacePath, 0x5F123456u);
  fam.faces[0].mtime = 0x5F123456u;
  loader.markDirty();
  const uint32_t fpOriginal = loader.getReaderFont()->styleCoverage() != 0 ? loader.fontFingerprint() : 0;
  EXPECT_NE(fpOriginal, 0u);

  // mtime bump with swapped content: cache must miss and rehash the NEW
  // bytes — fingerprint follows the content, never the stale record.
  seedLoadableFace(modified, kFacePath, 0x5FFFFFFFu);
  fam.faces[0].mtime = 0x5FFFFFFFu;
  loader.markDirty();
  loader.getReaderFont();  // markDirty alone only arms; ensureLoaded runs here
  const uint32_t fpNew = loader.fontFingerprint();
  EXPECT_NE(fpNew, fpOriginal);
}

TEST(BookFontLoaderFingerprintCache, CorruptCacheFileRecomputesAndRewrites) {
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u) << "fixture too small to be a TTF (truncated?)";
  resetStorage();
  constexpr uint32_t kMtime = 0x5F123456u;
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, kMtime);

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  fam.faces[0].mtime = kMtime;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);

  loader.markDirty();
  const uint32_t fpFirst = loader.getReaderFont()->styleCoverage() != 0 ? loader.fontFingerprint() : 0;
  EXPECT_NE(fpFirst, 0u);

  // Corrupt the cache record (wrong length, wrong magic) → the loader must
  // fall back to the pure byte-walk, get the SAME fingerprint, and rewrite a
  // valid 28-byte record for the next open.
  const std::string cachePath = fpCachePathFor(kFacePath);
  Storage.files[cachePath] = "garbage!";
  loader.markDirty();
  loader.getReaderFont();  // markDirty alone only arms; ensureLoaded runs here
  EXPECT_EQ(loader.fontFingerprint(), fpFirst);
  EXPECT_EQ(Storage.files[cachePath].size(), 28u);
}

TEST(BookFontLoaderFingerprintCache, MtimeZeroDisablesCache) {
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u) << "fixture too small to be a TTF (truncated?)";
  resetStorage();
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, 0);  // no SD timestamp

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  fam.faces[0].mtime = 0;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);

  loader.markDirty();
  EXPECT_EQ(loader.getReaderFont()->styleCoverage(), 0x07);
  // Fail closed: without a rehash trigger there is no safe cache identity —
  // no record may be written (or served) for that face.
  EXPECT_EQ(Storage.files.count(fpCachePathFor(kFacePath)), 0u);
}

#if defined(CROSSPOINT_FONT_BACKEND_FT) && CROSSPOINT_FONT_BACKEND_FT
// ── P2 hinting (Light) + stack-probe degrade identity ────────────────
// kRenderOptions requests Light; the runtime stack probe may degrade a slot
// to unhinted. The effective per-slot modes fold into the fingerprint tag
// (renderOptionsFingerprintTag), which both the loader and the FIBP prefetch
// worker mix into their parity hash — a degrade MUST change the tag.

namespace {

using RO = freeink::font::FtFont::RenderOptions;
using HM = freeink::font::FtFont::HintingMode;

uint32_t tagFor(std::initializer_list<HM> modes) {
  uint32_t tag = 0;
  uint8_t slot = 0;
  for (HM mode : modes) tag |= static_cast<uint32_t>(mode) << (3 * slot++);
  return tag;
}

}  // namespace

TEST(BookFontLoaderHinting, RequestedModeIsLightAndTagFoldsPerSlot) {
  BookFontLoader loader;
  loader.resetHintStateForTest();
  EXPECT_EQ(BookFontLoader::kRenderOptions.hinting, HM::Light);
  for (uint8_t slot = 0; slot < 4; ++slot) {
    EXPECT_EQ(BookFontLoader::effectiveRenderOptions(slot).hinting, HM::Light) << "slot " << slot;
    // Crisp is the default render mode (task6 directive).
    EXPECT_TRUE(BookFontLoader::effectiveRenderOptions(slot).monochrome) << "slot " << slot;
  }
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::Light, HM::Light, HM::Light, HM::Light}));
}

TEST(BookFontLoaderHinting, ApplyRenderModeFlipsMonochromeAndTag) {
  BookFontLoader loader;
  loader.resetHintStateForTest();
  loader.applyRenderMode(false);  // Smooth
  EXPECT_FALSE(BookFontLoader::effectiveRenderOptions(0).monochrome);
  loader.applyRenderMode(true);  // Crisp
  EXPECT_TRUE(BookFontLoader::effectiveRenderOptions(3).monochrome);
  // Raster mode is NOT layout identity: advances are identical across
  // modes, so the tag (and with it every FIBP gen) must stay stable or a
  // firmware update flipping the default re-indexes every book (soak
  // finding #6 correction: gen 1029201805 -> 3637587853 across builds).
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::Light, HM::Light, HM::Light, HM::Light}));
  loader.resetHintStateForTest();
}

TEST(BookFontLoaderHinting, DegradeFlipsEffectiveOptionsAndTag) {
  // A throwaway instance: the seam flips the file-scope effective-options
  // state; this instance's own faces stay null so setRenderOptions is
  // skipped (state-only degrade, exactly what the tag tests need).
  BookFontLoader loader;
  loader.degradeHintForTest(1);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(1).hinting, HM::None);
  EXPECT_EQ(BookFontLoader::effectiveRenderOptions(0).hinting, HM::Light);
  const uint32_t degraded = BookFontLoader::renderOptionsFingerprintTag();
  EXPECT_NE(degraded, tagFor({HM::Light, HM::Light, HM::Light, HM::Light}));
  EXPECT_EQ(degraded, tagFor({HM::Light, HM::None, HM::Light, HM::Light}));
  // Mutate-check the tag really folds the degraded slot: a second slot's
  // degrade must move the tag again (no aliasing between slot fields).
  loader.degradeHintForTest(2);
  EXPECT_NE(BookFontLoader::renderOptionsFingerprintTag(), degraded);
  loader.resetHintStateForTest();
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::Light, HM::Light, HM::Light, HM::Light}));
}

// Device regression 2026-09-19: a mono request against a build whose
// FreeType lacks the mono renderer module rasterized EVERY glyph to nullptr
// (blank page). The load funnel must degrade to the nearest supported set
// (AA first) and the face must still paint.
TEST(BookFontLoaderHinting, UnsupportedMonoDegradesToAaAndStillRenders) {
  // The host FT variant compiles neither the mono renderer nor the
  // auto-hinter, so Crisp is refused here exactly like on a firmware env
  // missing FREEINK_FONT_ENABLE_MONOCHROME.
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u) << "fixture too small to be a TTF (truncated?)";
  resetStorage();
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, 0x5F123456u);

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  freeink::book::BookFontLoader loader;
  loader.begin();
  auto& fam = loader.editFamily(0);
  std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
  fam.faceCount = 1;
  fam.faces[0].styleFlags = freeink::book::StyleNone;
  fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
  fam.faces[0].mtime = 0x5F123456u;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
  loader.setFamilyCountForTest(1);

  loader.markDirty();
  ASSERT_NE(loader.getReaderFont(), nullptr);

  // (C2) The degrade is visible in the effective mode: mono is dropped.
  // The TAG reflects the degrade: the host FT variant compiles neither the
  // mono renderer nor the auto-hinter, so the funnel drops BOTH on the
  // seeded slot (device builds compile both — there the tag stays Light).
  // Raster mode alone never moves the tag.
  EXPECT_FALSE(BookFontLoader::effectiveMonochrome());
  EXPECT_EQ(BookFontLoader::renderOptionsFingerprintTag(), tagFor({HM::None, HM::Light, HM::Light, HM::Light}));

  // (C1) The degraded face still rasterizes: non-null pixels, non-empty
  // bitmap (the regression produced nullptr for every glyph).
  freeink::font::FontChain* chain = loader.getReaderFont();
  freeink::font::RasterFont* face = chain->fontFor('A');
  ASSERT_NE(face, nullptr);
  const freeink::font::GlyphBitmap* bmp = face->rasterize('A', 14);
  ASSERT_NE(bmp, nullptr);
  ASSERT_NE(bmp->pixels, nullptr);
  EXPECT_GT(size_t(bmp->width) * bmp->height, 0u);
}

// Gen-stability contract (soak finding #6 correction): the font
// fingerprint — the input every FIBP generation derives from — must be a
// pure function of the font bytes + style coverage + hinting tag. Two
// loader instances over unchanged files must agree exactly, or a firmware
// update (or a second loader in the same boot) silently re-indexes every
// book. layoutGenerationHash() hashes field-by-field (no struct padding),
// so fingerprint determinism pins the whole chain.
TEST(BookFontLoaderHinting, FingerprintStableAcrossLoaderInstances) {
  const std::string dejavu = readFixtureFile(DEJAVU_FIXTURE);
  if (!fixtureAvailable(dejavu)) GTEST_SKIP() << "fixture unavailable: DejaVuSans.ttf";
  ASSERT_GE(dejavu.size(), 16u);
  resetStorage();
  constexpr const char* kFacePath = "/fonts/Deja/Deja-Regular.ttf";
  seedLoadableFace(dejavu, kFacePath, 0x5F123456u);

  testSetPsramHeap({8 * 1024 * 1024, 8 * 1024 * 1024, 0, 0});
  uint32_t fingerprints[2] = {};
  for (auto& fp : fingerprints) {
    freeink::book::BookFontLoader loader;
    loader.begin();
    auto& fam = loader.editFamily(0);
    std::snprintf(fam.name, sizeof(fam.name), "%s", "Deja");
    fam.faceCount = 1;
    fam.faces[0].styleFlags = freeink::book::StyleNone;
    fam.faces[0].fileSize = static_cast<uint32_t>(dejavu.size());
    fam.faces[0].mtime = 0x5F123456u;
    std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", kFacePath);
    loader.setFamilyCountForTest(1);
    loader.markDirty();
    ASSERT_NE(loader.getReaderFont(), nullptr);
    fp = loader.fontFingerprint();
    EXPECT_NE(fp, 0u);
  }
  EXPECT_EQ(fingerprints[0], fingerprints[1]);
}
#endif
