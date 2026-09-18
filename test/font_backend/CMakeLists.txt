# Per-backend font invariant tests (design D9) — built as both variants
# (stb + FreeType) by add_font_backend_tests(); see test/CMakeLists.txt.
add_font_backend_tests(FontBackendInvariantsTest
  SOURCES
    FontBackendInvariantsTest.cpp
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/TtfFont.cpp
  INCLUDES
    ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include
    ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/third_party/stb
  DEFINITIONS
    DEJAVU_FIXTURE="${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/test/fixtures/fonts/DejaVuSans.ttf"
    EMBER_FIXTURES_DIR="${REPO_ROOT}/test/fixtures/fonts/amazon-ember"
    ATKINSON_OTF_FIXTURE="${REPO_ROOT}/test/fixtures/fonts/atkinson-hyperlegible-next/AtkinsonHyperlegibleNext-Regular.otf"
    VF_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/RedHatDisplayVariable.ttf"
)
