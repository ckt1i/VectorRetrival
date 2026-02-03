# ============================================================================
# VectorDB Tests CMakeLists.txt
# ============================================================================

# ============================================================================
# Common Test Utilities
# ============================================================================
add_library(vdb_test_utils INTERFACE)
target_link_libraries(vdb_test_utils INTERFACE
  GTest::gtest
  GTest::gtest_main
  vdb_common
)

# ============================================================================
# Unit Tests
# ============================================================================

# Common module tests
add_executable(types_test
  common/types_test.cpp
)
target_link_libraries(types_test PRIVATE vdb_test_utils)
gtest_discover_tests(types_test)

add_executable(status_test
  common/status_test.cpp
)
target_link_libraries(status_test PRIVATE vdb_test_utils)
gtest_discover_tests(status_test)

add_executable(aligned_alloc_test
  common/aligned_alloc_test.cpp
)
target_link_libraries(aligned_alloc_test PRIVATE vdb_test_utils)
gtest_discover_tests(aligned_alloc_test)

# ============================================================================
# Add all tests to a single target for convenience
# ============================================================================
add_custom_target(all_tests
  DEPENDS
    types_test
    status_test
    aligned_alloc_test
)
