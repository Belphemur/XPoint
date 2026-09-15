add_executable(HomeButtonInputTest HomeButtonInputTest.cpp)

target_include_directories(HomeButtonInputTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(HomeButtonInputTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HomeButtonInputTest)
