add_executable(ProgressManagerTest
  ProgressManagerTest.cpp
  stubs/Stubs.cpp
  ${REPO_ROOT}/src/ProgressManager.cpp
)

# The stubs directory must come FIRST so <freertos/...>, <HalStorage.h> and
# <HalPowerManager.h> resolve to the host-safe stubs instead of the firmware
# headers (same pattern as test/book_font_loader).
target_include_directories(ProgressManagerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(ProgressManagerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressManagerTest)
