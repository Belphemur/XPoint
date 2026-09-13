add_executable(PsramImageStagingTest
  PsramImageStagingTest.cpp
)

target_include_directories(PsramImageStagingTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(PsramImageStagingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PsramImageStagingTest)