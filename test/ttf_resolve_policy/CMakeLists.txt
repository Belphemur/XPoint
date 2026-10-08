add_executable(TtfResolvePolicyTest
  TtfResolvePolicyTest.cpp
  ../../src/activities/reader/TtfResolvePolicy.cpp
)

target_include_directories(TtfResolvePolicyTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib
)

target_compile_definitions(TtfResolvePolicyTest PRIVATE CROSSPOINT_TTF_READER=1)

target_link_libraries(TtfResolvePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TtfResolvePolicyTest)
