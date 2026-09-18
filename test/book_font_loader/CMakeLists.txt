# Both font-backend variants (stb + FreeType) — see add_font_backend_tests().
# The FT variant exercises the loader's FreeType path (tryLoadFace constructs
# FtFont, no glyph arena); the direct-TtfFont assertions inside the test body
# stay stb-based in both variants (TtfFont.cpp is always compiled in).
add_font_backend_tests(BookFontLoaderTest
  SOURCES
    BookFontLoaderTest.cpp
    ${REPO_ROOT}/src/BookFontLoader.cpp
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/TtfFont.cpp
    stubs/Stubs.cpp
  INCLUDES
    # stubs/ first so Arduino.h/HalStorage.h resolve to the host-safe versions
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${REPO_ROOT}/src
    ${REPO_ROOT}/lib
    ${REPO_ROOT}/lib/Memory
    ${REPO_ROOT}/lib/hal
    ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/third_party/stb
    ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/stb
    ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
  DEFINITIONS
    # Memory.h host mode (no heap_caps); matches the real host-test gating.
    HOST_TEST=1
    # Real-font fixtures for the scan/upem/subset tests (paths, not contents).
    EMBER_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/amazon-ember"
    DEJAVU_FIXTURE="${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/test/fixtures/fonts/DejaVuSans.ttf"
    ATKINSON_OTF_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/atkinson-hyperlegible-next/AtkinsonHyperlegibleNext-Regular.otf"
)
