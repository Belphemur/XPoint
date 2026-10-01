cmake_minimum_required(VERSION 3.16)

add_executable(PowerTest
  PowerTest.cpp
  ${REPO_ROOT}/lib/power/PowerDrainMonitor.cpp
)
target_include_directories(PowerTest PRIVATE
  ${REPO_ROOT}/lib/power
)
target_link_libraries(PowerTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PowerTest)
