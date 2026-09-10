add_executable(ProgressFlushTest
  ProgressFlushTest.cpp
)

target_link_libraries(ProgressFlushTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressFlushTest)
