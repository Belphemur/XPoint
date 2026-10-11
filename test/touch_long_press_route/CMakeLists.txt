add_executable(TouchLongPressRouteTest TouchLongPressRouteTest.cpp)

target_link_libraries(TouchLongPressRouteTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TouchLongPressRouteTest)
