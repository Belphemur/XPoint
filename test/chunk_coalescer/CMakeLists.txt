add_executable(ChunkCoalescerTest
  ChunkCoalescerTest.cpp
)
target_include_directories(ChunkCoalescerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Download
)
target_link_libraries(ChunkCoalescerTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ChunkCoalescerTest)
