set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(ReadingStatsStoreTest
  ReadingStatsBinaryStoreTest.cpp
  ReadingStatsUtilsTest.cpp
  ${REPO_ROOT}/src/activities/reader/FinishedBooksIndex.cpp
  ${REPO_ROOT}/src/activities/reader/GlobalReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/BookReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
)

target_include_directories(ReadingStatsStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/shims
  ${CMAKE_CURRENT_SOURCE_DIR}/../minibidi_arabic/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/../fs_helpers/stubs
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/src/activities/reader
)

target_compile_definitions(ReadingStatsStoreTest PRIVATE READING_STATS_ENABLED=1 READING_STATS_TEST=1)

target_link_libraries(ReadingStatsStoreTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(ReadingStatsStoreTest)
