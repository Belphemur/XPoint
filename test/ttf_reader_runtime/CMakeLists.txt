add_executable(TtfReaderRuntimeTest
  TtfReaderRuntimeTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/src/cache/PageCache.cpp
)

target_include_directories(TtfReaderRuntimeTest PRIVATE
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
  ${REPO_ROOT}/src
)

target_link_libraries(TtfReaderRuntimeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TtfReaderRuntimeTest)
