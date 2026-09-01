add_executable(OtaJsonParserTest
  OtaJsonParserTest.cpp
  ${REPO_ROOT}/src/network/OtaJson.cpp
)

target_include_directories(OtaJsonParserTest PRIVATE
  ${REPO_ROOT}/src/network
  ${REPO_ROOT}/lib/OtaSignature
)

target_link_libraries(OtaJsonParserTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(OtaJsonParserTest)
