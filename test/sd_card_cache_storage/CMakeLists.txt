add_executable(SdCardCacheStorageTest
  SdCardCacheStorageTest.cpp
  ${REPO_ROOT}/src/adapters/SdCardCacheStorage.cpp
  stubs/Stubs.cpp
)

target_include_directories(SdCardCacheStorageTest PRIVATE
  # stubs/ first so HalStorage.h/HalFile.h resolve to the host-safe versions
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/adapters
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/freeink-sdk/libs/book/FreeInkBook/include
)

target_compile_definitions(SdCardCacheStorageTest PRIVATE
  HOST_TEST=1
)

target_link_libraries(SdCardCacheStorageTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SdCardCacheStorageTest)