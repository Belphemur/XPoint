add_executable(ChapterIndexEngineTest
  ChapterIndexEngineTest.cpp
  ${REPO_ROOT}/src/activities/reader/ChapterIndexEngine.cpp
)

target_include_directories(ChapterIndexEngineTest PRIVATE
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/include
  ${REPO_ROOT}/src
)

# The engine is part of the native-TTF reader surface (guarded like
# TtfBookRuntime); the host build needs the same feature define.
target_compile_definitions(ChapterIndexEngineTest PRIVATE CROSSPOINT_TTF_READER=1)

target_link_libraries(ChapterIndexEngineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChapterIndexEngineTest)
