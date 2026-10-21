set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(FinishReadingPromptPolicyTest
  FinishReadingPromptPolicyTest.cpp
  ${REPO_ROOT}/src/activities/reader/FinishReadingPromptPolicy.cpp
)

target_include_directories(FinishReadingPromptPolicyTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(FinishReadingPromptPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FinishReadingPromptPolicyTest)
