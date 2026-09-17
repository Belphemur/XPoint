# Real upstream CrossPoint release image (5 MB) — too big to commit. Fetch it
# once at configure time and verify against the pinned SHA-256; a corrupt or
# missing download is re-fetched on the next configure. Override the URL for
# offline/air-gapped environments with -DX4PRO_FIXTURE_URL=file:///path.bin.
set(X4PRO_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/crosspoint-1.6.0-x4pro.bin")
set(X4PRO_FIXTURE_URL "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/1.6.0/crosspoint-1.6.0-x4pro.bin" CACHE STRING
    "Download URL for the upstream firmware validation fixture")
set(X4PRO_FIXTURE_SHA256 "d20f91502e779a2ba68d9c2423da1a201d74c55badd47f79f84400a320333167")

if(EXISTS "${X4PRO_FIXTURE}")
  file(SHA256 "${X4PRO_FIXTURE}" x4pro_fixture_hash)
  if(NOT x4pro_fixture_hash STREQUAL X4PRO_FIXTURE_SHA256)
    message(STATUS "Firmware fixture hash mismatch; re-downloading")
    file(REMOVE "${X4PRO_FIXTURE}")
  endif()
endif()
if(NOT EXISTS "${X4PRO_FIXTURE}")
  message(STATUS "Downloading upstream CrossPoint 1.6.0 x4pro firmware fixture")
  file(DOWNLOAD "${X4PRO_FIXTURE_URL}" "${X4PRO_FIXTURE}"
       EXPECTED_HASH SHA256=${X4PRO_FIXTURE_SHA256}
       TIMEOUT 600 SHOW_PROGRESS)
endif()

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
  # Enables the per-call chip-id read in runningPartitionChipId() so tests can
  # re-point it between fixture images (firmware keeps the boot-time cache).
  HOST_TEST=1
  X4PRO_FIXTURE_PATH="${X4PRO_FIXTURE}"
)

target_link_libraries(FirmwareFlasherTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareFlasherTest)
