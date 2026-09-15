add_executable(UiAppHandlersTest
  UiAppHandlersTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp
)

target_include_directories(UiAppHandlersTest PRIVATE
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
)

target_link_libraries(UiAppHandlersTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UiAppHandlersTest)
