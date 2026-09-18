add_executable(FibpPrefetchPolicyTest
  FibpPrefetchPolicyTest.cpp
)

target_include_directories(FibpPrefetchPolicyTest PRIVATE
  ${REPO_ROOT}/src
)

# Policy layer is part of the native-TTF reader surface; the host build needs
# the same feature define (the worker class itself stays an inert stub).
target_compile_definitions(FibpPrefetchPolicyTest PRIVATE CROSSPOINT_TTF_READER=1)

target_link_libraries(FibpPrefetchPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FibpPrefetchPolicyTest)
