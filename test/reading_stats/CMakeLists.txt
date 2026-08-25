set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

include(FetchContent)

FetchContent_Declare(
  sqlite_amalgamation
  URL https://www.sqlite.org/2025/sqlite-amalgamation-3490100.zip
  URL_HASH SHA256=6cebd1d8403fc58c30e93939b246f3e6e58d0765a5cd50546f16c00fd805d2c3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite_amalgamation)

# The Sqlite3Esp32 amalgamation hard-includes its ESP32-tuned config_ext.h
# (SQLITE_OS_OTHER=1). For the host test we build a stock SQLite (unix VFS)
# from a fetched amalgamation next to a neutral config_ext.h, then register our
# "hal" VFS on top. The "hal" VFS host branch is POSIX-backed, matching the
# design's "host build uses POSIX backend behind the same interface".
set(HOST_SQLITE_DIR "${CMAKE_CURRENT_BINARY_DIR}/host_sqlite")
file(MAKE_DIRECTORY "${HOST_SQLITE_DIR}")
configure_file("${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c" "${HOST_SQLITE_DIR}/sqlite3.c" COPYONLY)
configure_file("${sqlite_amalgamation_SOURCE_DIR}/sqlite3.h" "${HOST_SQLITE_DIR}/sqlite3.h" COPYONLY)
file(WRITE "${HOST_SQLITE_DIR}/config_ext.h" "/* host test: use stock SQLite unix backend */\n")

add_executable(ReadingStatsStoreTest
  ReadingStatsStoreTest.cpp
  ${HOST_SQLITE_DIR}/sqlite3.c
  ${REPO_ROOT}/lib/SQLite/SQLiteVfsHal.cpp
  ${REPO_ROOT}/lib/SQLite/ReadingStatsStore.cpp
  ${REPO_ROOT}/src/activities/reader/GlobalReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/BookReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
)

target_include_directories(ReadingStatsStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/shims
  ${HOST_SQLITE_DIR}
  ${REPO_ROOT}/lib/SQLite
  ${REPO_ROOT}/src/activities/reader
)

target_compile_definitions(ReadingStatsStoreTest PRIVATE READING_STATS_ENABLED=1)

target_link_libraries(ReadingStatsStoreTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(ReadingStatsStoreTest)
