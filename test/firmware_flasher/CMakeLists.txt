add_executable(FirmwareFlasherTest
  FirmwareFlasherTest.cpp
  ${REPO_ROOT}/src/network/FirmwareFlasher.cpp
  ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
  stubs/Stubs.cpp
)

target_include_directories(FirmwareFlasherTest PRIVATE
  # stubs/ first so ESP-IDF, mbedTLS, HAL, and logging headers resolve to the
  # host-safe versions.
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/network
)

target_compile_definitions(FirmwareFlasherTest PRIVATE
  FREEINK_DEVICE_X4PRO=1
)

target_link_libraries(FirmwareFlasherTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareFlasherTest)
