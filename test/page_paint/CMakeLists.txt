add_executable(PagePaintTest
  PagePaintTest.cpp
  ${REPO_ROOT}/src/adapters/PagePaint.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/render/TtfFont.cpp
)

target_include_directories(PagePaintTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/adapters
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/stb
)

target_compile_definitions(PagePaintTest PRIVATE
  CROSSPOINT_TTF_READER=1
  HOST_TEST=1
)

target_link_libraries(PagePaintTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PagePaintTest)
