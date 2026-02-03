cmake_minimum_required(VERSION 3.16)
project(vectordb VERSION 0.1.0 LANGUAGES CXX)

# =============================================================================
# Build Options
# =============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VDB_BUILD_TESTS "Build tests" ON)
option(VDB_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(VDB_BUILD_TOOLS "Build tools" OFF)
option(VDB_USE_AVX2 "Enable AVX2 SIMD" ON)
option(VDB_USE_AVX512 "Enable AVX-512 SIMD" OFF)

# =============================================================================
# Compiler Flags
# =============================================================================
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

if(VDB_USE_AVX512)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx512f -mavx512dq -mavx512bw -mavx512vl")
    add_compile_definitions(VDB_USE_AVX512=1)
elseif(VDB_USE_AVX2)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2 -mfma")
    add_compile_definitions(VDB_USE_AVX2=1)
endif()

# =============================================================================
# Dependencies (FetchContent)
# =============================================================================
include(FetchContent)
find_package(Threads REQUIRED)

# FlatBuffers - build from source to get flatc compiler
FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG v24.3.25
)
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(flatbuffers)

# Google Test (for tests)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# =============================================================================
# FlatBuffers Code Generation
# =============================================================================
set(FLATBUFFERS_SCHEMA_DIR ${CMAKE_SOURCE_DIR}/schema)
set(FLATBUFFERS_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)

file(MAKE_DIRECTORY ${FLATBUFFERS_GENERATED_DIR})

# Schema files
set(FBS_FILES
    ${FLATBUFFERS_SCHEMA_DIR}/columns.fbs
    ${FLATBUFFERS_SCHEMA_DIR}/segment_meta.fbs
)

# Generate headers using the flatc we just built
foreach(FBS_FILE ${FBS_FILES})
    get_filename_component(FBS_NAME ${FBS_FILE} NAME_WE)
    set(GENERATED_HEADER ${FLATBUFFERS_GENERATED_DIR}/${FBS_NAME}_generated.h)
    
    add_custom_command(
        OUTPUT ${GENERATED_HEADER}
        COMMAND flatc --cpp --scoped-enums -o ${FLATBUFFERS_GENERATED_DIR} ${FBS_FILE}
        DEPENDS ${FBS_FILE} flatc
        COMMENT "Generating FlatBuffers header for ${FBS_NAME}"
    )
    list(APPEND GENERATED_HEADERS ${GENERATED_HEADER})
endforeach()

add_custom_target(flatbuffers_generated DEPENDS ${GENERATED_HEADERS})

# =============================================================================
# VDB Common Library (header-only for now)
# =============================================================================
add_library(vdb_common INTERFACE)
target_include_directories(vdb_common INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# =============================================================================
# VDB Schema Library (FlatBuffers generated headers)
# =============================================================================
add_library(vdb_schema INTERFACE)
target_include_directories(vdb_schema INTERFACE
    $<BUILD_INTERFACE:${FLATBUFFERS_GENERATED_DIR}>
)
target_link_libraries(vdb_schema INTERFACE flatbuffers)
add_dependencies(vdb_schema flatbuffers_generated)

# =============================================================================
# Tests
# =============================================================================
if(VDB_BUILD_TESTS)
    enable_testing()
    
    # -------------------------------------------------------------------------
    # Common module tests (Phase 0)
    # -------------------------------------------------------------------------
    add_executable(test_types tests/common/types_test.cpp)
    target_link_libraries(test_types PRIVATE vdb_common GTest::gtest GTest::gtest_main)
    add_test(NAME test_types COMMAND test_types)
    
    add_executable(test_status tests/common/status_test.cpp)
    target_link_libraries(test_status PRIVATE vdb_common GTest::gtest GTest::gtest_main)
    add_test(NAME test_status COMMAND test_status)
    
    add_executable(test_aligned_alloc tests/common/aligned_alloc_test.cpp)
    target_link_libraries(test_aligned_alloc PRIVATE vdb_common GTest::gtest GTest::gtest_main)
    add_test(NAME test_aligned_alloc COMMAND test_aligned_alloc)
    
    # -------------------------------------------------------------------------
    # Schema tests (Phase 1)
    # -------------------------------------------------------------------------
    add_executable(test_schema tests/schema/schema_test.cpp)
    target_link_libraries(test_schema PRIVATE vdb_common vdb_schema GTest::gtest GTest::gtest_main)
    add_dependencies(test_schema flatbuffers_generated)
    add_test(NAME test_schema COMMAND test_schema)
endif()

# =============================================================================
# Summary
# =============================================================================
message(STATUS "")
message(STATUS "VectorDB Configuration Summary:")
message(STATUS "  Version:      ${PROJECT_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  AVX2:         ${VDB_USE_AVX2}")
message(STATUS "  AVX-512:      ${VDB_USE_AVX512}")
message(STATUS "  Tests:        ${VDB_BUILD_TESTS}")
message(STATUS "  Benchmarks:   ${VDB_BUILD_BENCHMARKS}")
message(STATUS "  Tools:        ${VDB_BUILD_TOOLS}")
message(STATUS "")
