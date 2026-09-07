add_executable(DictionarySelectionTest
  DictionarySelectionTest.cpp
  ${REPO_ROOT}/src/util/DictionarySelection.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(DictionarySelectionTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(DictionarySelectionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictionarySelectionTest)
