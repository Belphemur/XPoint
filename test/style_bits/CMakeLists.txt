add_executable(StyleBitsTest
  StyleBitsTest.cpp
)

target_include_directories(StyleBitsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
)

target_link_libraries(StyleBitsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StyleBitsTest)