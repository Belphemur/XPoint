set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

file(GLOB SQLITE3_SRC "${REPO_ROOT}/.pio/libdeps/*/Sqlite3Esp32/src/sqlite3.c")
if(NOT SQLITE3_SRC)
  message(FATAL_ERROR "sqlite3.c not found; run `pio run -e x4pro` first to populate libdeps")
endif()
list(GET SQLITE3_SRC 0 SQLITE3_SRC_FIRST)
get_filename_component(SQLITE3_INC_DIR "${SQLITE3_SRC_FIRST}" DIRECTORY)

# The Sqlite3Esp32 amalgamation hard-includes its ESP32-tuned config_ext.h
# (SQLITE_OS_OTHER=1). For the host test we build a stock SQLite (unix VFS)
# from a copied amalgamation next to a neutral config_ext.h, then register our
# "hal" VFS on top. The "hal" VFS host branch is POSIX-backed, matching the
# design's "host build uses POSIX backend behind the same interface".
set(HOST_SQLITE_DIR "${CMAKE_CURRENT_BINARY_DIR}/host_sqlite")
file(MAKE_DIRECTORY "${HOST_SQLITE_DIR}")
configure_file("${SQLITE3_INC_DIR}/sqlite3.c" "${HOST_SQLITE_DIR}/sqlite3.c" COPYONLY)
configure_file("${SQLITE3_INC_DIR}/sqlite3.h" "${HOST_SQLITE_DIR}/sqlite3.h" COPYONLY)
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
