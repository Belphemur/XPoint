add_executable(HomeButtonInputTest HomeButtonInputTest.cpp)
add_executable(HomeButtonMigrationTest HomeButtonMigrationTest.cpp)

target_include_directories(HomeButtonInputTest PRIVATE
  ${REPO_ROOT}/src
)
target_include_directories(HomeButtonMigrationTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(HomeButtonInputTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
target_link_libraries(HomeButtonMigrationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HomeButtonInputTest)
gtest_discover_tests(HomeButtonMigrationTest)
