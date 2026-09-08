add_executable(MiniBidiArabicTest
  MiniBidiArabicTest.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# Vendored upstream C source emits -Wsign-compare under the test build's
# -Wall/-Wextra; leave the vendored code untouched and silence it here.
set_source_files_properties(${REPO_ROOT}/lib/MiniBidi/minibidi.c PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare")

target_include_directories(MiniBidiArabicTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs  # no-op Logging.h (real one needs Arduino)
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(MiniBidiArabicTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MiniBidiArabicTest)
