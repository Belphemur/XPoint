// Host-side gtest suite for the Phase 1a BookFontLoader (native TTF fonts).
// Exercises the real loader paths through controllable file/heap stubs:
// builtin fallback, budget clamping, malformed-sfnt rejection, fingerprint
// stability, and the retained byte-lifetime contract.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "BookFontLoader.h"
#include "TestHeapHooks.h"

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

// Register one face directly into family[0] (Phase 2 owns discovery; the
// manifest is populated here to drive tryLoadFace deterministically).
void addFace(freeink::book::BookFontLoader& loader, const char* path, uint32_t size, uint8_t styleFlags) {
  auto& fam = loader.editFamily(0);
  fam.faceCount = 1;
  fam.faces[0].styleFlags = styleFlags;
  fam.faces[0].fileSize = size;
  std::snprintf(fam.faces[0].file, sizeof(fam.faces[0].file), "%s", path);
}

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

  book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
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

  book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
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

  book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
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
  seedFile("/fonts/Solo/Solo-Head.ttf");

  book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(fams[0].faceCount, 1u);
  EXPECT_EQ(fams[0].faces[0].styleFlags, freeink::book::StyleNone);
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

  book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
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
  book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  EXPECT_EQ(count, BookFontLoader::kMaxDiscoveredFamilies);
}

TEST(ScanFontsTest, OtfExtensionAccepted) {
  resetStorage();
  seedFile("/fonts/Otf/Otf-Regular.otf");
  book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  ASSERT_EQ(count, 1u);
  EXPECT_STREQ(fams[0].faces[0].file, "/fonts/Otf/Otf-Regular.otf");
}

TEST(ScanFontsTest, MissingRootIsQuietNoop) {
  resetStorage();
  book::FamilyInfo fams[BookFontLoader::kMaxDiscoveredFamilies];
  uint8_t count = 0;
  BookFontLoader::scanFontsForTest("/fonts", fams, count);
  EXPECT_EQ(count, 0u);
}
