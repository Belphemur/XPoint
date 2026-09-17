add_executable(ActivityResultTest
  ActivityResultTest.cpp
)

target_include_directories(ActivityResultTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ActivityResultTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ActivityResultTest)
