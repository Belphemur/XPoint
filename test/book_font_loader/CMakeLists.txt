add_executable(BookFontLoaderTest
  BookFontLoaderTest.cpp
  ${REPO_ROOT}/src/BookFontLoader.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/render/TtfFont.cpp
  stubs/Stubs.cpp
)

target_include_directories(BookFontLoaderTest PRIVATE
  # stubs/ first so Arduino.h/HalStorage.h resolve to the host-safe versions
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/third_party/stb
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
)

target_compile_definitions(BookFontLoaderTest PRIVATE
  # Memory.h host mode (no heap_caps); matches the real host-test gating.
  $<$<NOT:$<BOOL:${ESP_PLATFORM}>>:HOST_TEST=1>
)

target_link_libraries(BookFontLoaderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookFontLoaderTest)