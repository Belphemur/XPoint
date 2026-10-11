add_executable(DictionaryDefinitionRoutingTest
  DictionaryDefinitionRoutingTest.cpp
)

target_link_libraries(DictionaryDefinitionRoutingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictionaryDefinitionRoutingTest)
