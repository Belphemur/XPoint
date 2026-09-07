add_executable(GrayPlanesTest
  GrayPlanesTest.cpp
)

target_link_libraries(GrayPlanesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GrayPlanesTest)
