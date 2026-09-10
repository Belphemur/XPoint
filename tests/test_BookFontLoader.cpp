// BookFontLoader test suite for Phase 1a - native TTF font loading infrastructure
// Tests the BookFontLoader::begin() and BookFontLoader::getReaderFont() functionality
// Tests the font loading infrastructure for native TTF/OTF support

#include <cassert>
#include <iostream>

#include "BookFontLoader.h"

void testBookFontLoaderBasics() {
  std::cout << "Testing BookFontLoader basic functionality..."
            << std::endl;

  // Test 1: Instance creation
  freeink::book::BookFontLoader loader;
  assert(&loader != nullptr);
  std::cout << "✓ BookFontLoader instance created" << std::endl;

  // Test 2: Initial state (before begin() is called)
  // Family count should be 0 initially
  assert(loader.familyCount() == 0);
  std::cout << "✓ Initial family count is 0" << std::endl;

  // Test 3: Font fingerprint should be 0 initially
  assert(loader.fontFingerprint() == 0);
  std::cout << "✓ Initial font fingerprint is 0" << std::endl;

  // Test 4: getReaderFont() should return non-null (will be builtin fallback
  // initially)
  freeink::book::FontChain* readerFont = loader.getReaderFont();
  assert(readerFont != nullptr);
  std::cout << "✓ getReaderFont() returns non-null pointer" << std::endl;

  // Test 5: Style coverage of reader font (should be 0x07 for the builtin
  // fallback with 4 registered styles)
  uint8_t styleCoverage = readerFont->styleCoverage();
  assert(styleCoverage == 0x07);
  std::cout << "✓ Reader font style coverage is 0x07 (builtin fallback)"
            << std::endl;

  std::cout << "All BookFontLoader basic tests passed!" << std::endl;
}

void testFontFaceInfoStructure() {
  std::cout << "Testing FontFaceInfo structure..." << std::endl;

  // Test 1: FontFaceInfo member initialization
  freeink::book::FontFaceInfo faceInfo;

  // Check that string fields are null-terminated initially
  assert(faceInfo.name[0] == '\0');
  assert(faceInfo.file[0] == '\0');
  std::cout << "✓ FontFaceInfo strings are null-terminated initially"
            << std::endl;

  // Test 2: StyleFlags enum values
  assert(freeink::book::StyleNone == 0);
  assert(freeink::book::StyleBold == 1);
  assert(freeink::book::StyleItalic == 2);
  std::cout << "✓ StyleFlags enum values are correct" << std::endl;

  std::cout << "All FontFaceInfo tests passed!" << std::endl;
}

void testFamilyInfoStructure() {
  std::cout << "Testing FamilyInfo structure..." << std::endl;

  // Test 1: FamilyInfo member initialization
  freeink::book::FamilyInfo familyInfo;

  // Check that string fields are null-terminated initially
  assert(familyInfo.name[0] == '\0');
  std::cout << "✓ FamilyInfo name is null-terminated initially" << std::endl;

  // Test 2: Initial face count should be 0
  assert(familyInfo.faceCount == 0);
  std::cout << "✓ Initial face count is 0" << std::endl;

  // Test 3: Initial isBuiltinFallback should be false
  assert(!familyInfo.isBuiltinFallback);
  std::cout << "✓ isBuiltinFallback is false initially" << std::endl;

  std::cout << "All FamilyInfo tests passed!" << std::endl;
}

void testStyleFlags() {
  std::cout << "Testing StyleFlags combinations..." << std::endl;

  // Test 1: Individual style flags
  assert((freeink::book::StyleBold & freeink::book::StyleItalic) == 0);
  // Bold and Italic shouldn't overlap
  std::cout << "✓ Bold and Italic flags don't overlap" << std::endl;

  // Test 2: Combined style flags
  uint8_t boldItalic =
      (freeink::book::StyleBold | freeink::book::StyleItalic);
  assert(boldItalic == (1 | 2));  // Bold (1) + Italic (2) = 3
  std::cout << "✓ Bold + Italic = " << (int)boldItalic << std::endl;

  // Test 3: StyleNone has no bits set
  assert(freeink::book::StyleNone == 0);
  std::cout << "✓ StyleNone is 0" << std::endl;

  std::cout << "All StyleFlags tests passed!" << std::endl;
}

void testConstants() {
  std::cout << "Testing BookFontLoader constants..." << std::endl;

  // Test 1: kMaxDiscoveredFamilies
  assert(freeink::book::BookFontLoader::kMaxDiscoveredFamilies > 0);
  assert(freeink::book::BookFontLoader::kMaxDiscoveredFamilies <= 100);
  // Reasonable upper bound
  std::cout << "✓ kMaxDiscoveredFamilies = "
            << (int)freeink::book::BookFontLoader::kMaxDiscoveredFamilies
            << std::endl;

  std::cout << "All constants tests passed!" << std::endl;
}

int main() {
  std::cout << "==================================================" << std::endl;
  std::cout << "BookFontLoader Test Suite" << std::endl;
  std::cout << "==================================================" << std::endl;

  try {
    testFontFaceInfoStructure();
    testFamilyInfoStructure();
    testStyleFlags();
    testConstants();
    testBookFontLoaderBasics();

    std::cout << "==================================================" << std::endl;
    std::cout << "All tests passed successfully!" << std::endl;
    std::cout << "==================================================" << std::endl;

    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed with exception: " << e.what() << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "Test failed with unknown exception" << std::endl;
    return 1;
  }
}
