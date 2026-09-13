add_executable(PsramImageStagingTest
  PsramImageStagingTest.cpp
)

target_include_directories(PsramImageStagingTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../section_incremental/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(PsramImageStagingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PsramImageStagingTest)