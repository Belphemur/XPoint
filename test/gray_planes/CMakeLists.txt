add_executable(GrayPlanesTest
  GrayPlanesTest.cpp
)

target_include_directories(GrayPlanesTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
)

target_link_libraries(GrayPlanesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GrayPlanesTest)
