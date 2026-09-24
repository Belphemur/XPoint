add_executable(SleepFrameHashTest
  SleepFrameHashTest.cpp
)

target_include_directories(SleepFrameHashTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(SleepFrameHashTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepFrameHashTest)
