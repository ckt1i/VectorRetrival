cmake_minimum_required(VERSION 3.16)
project(vectordb VERSION 0.1.0 LANGUAGES CXX)

# =============================================================================
# Build Options
# =============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VDB_BUILD_TESTS "Build tests" ON)
option(VDB_BUILD_BENCHMARKS "Build benchmarks" ON)
option(VDB_BUILD_TOOLS "Build tools" ON)
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
# Dependencies
# =============================================================================
find_package(Threads REQUIRED)

# FlatBuffers
find_package(flatbuffers CONFIG QUIET)
if(NOT flatbuffers_FOUND)
    message(STATUS "FlatBuffers not found via CMake, will use system flatc")
endif()

# =============================================================================
# FlatBuffers Code Generation
# =============================================================================
set(FLATBUFFERS_SCHEMA_DIR ${CMAKE_SOURCE_DIR}/schema)
set(FLATBUFFERS_GENERATED_DIR ${CMAKE_SOURCE_DIR}/schema/generated)

file(MAKE_DIRECTORY ${FLATBUFFERS_GENERATED_DIR})

# Find flatc compiler
find_program(FLATC_EXECUTABLE flatc)
if(FLATC_EXECUTABLE)
    message(STATUS "Found flatc: ${FLATC_EXECUTABLE}")
    
    # Generate headers from .fbs files
    set(FBS_FILES
        ${FLATBUFFERS_SCHEMA_DIR}/segment_meta.fbs
        ${FLATBUFFERS_SCHEMA_DIR}/columns.fbs
    )
    
    foreach(FBS_FILE ${FBS_FILES})
        get_filename_component(FBS_NAME ${FBS_FILE} NAME_WE)
        set(GENERATED_HEADER ${FLATBUFFERS_GENERATED_DIR}/${FBS_NAME}_generated.h)
        
        add_custom_command(
            OUTPUT ${GENERATED_HEADER}
            COMMAND ${FLATC_EXECUTABLE} --cpp -o ${FLATBUFFERS_GENERATED_DIR} ${FBS_FILE}
            DEPENDS ${FBS_FILE}
            COMMENT "Generating FlatBuffers header for ${FBS_NAME}"
        )
        list(APPEND GENERATED_HEADERS ${GENERATED_HEADER})
    endforeach()
    
    add_custom_target(flatbuffers_generated DEPENDS ${GENERATED_HEADERS})
else()
    message(WARNING "flatc not found, FlatBuffers headers must be generated manually")
    add_custom_target(flatbuffers_generated)
endif()

# =============================================================================
# Include Directories
# =============================================================================
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${FLATBUFFERS_GENERATED_DIR}
)

# =============================================================================
# Source Files
# =============================================================================
set(VDB_COMMON_SOURCES
    src/common/status.cpp
    src/common/crc32.cpp
    src/common/thread_pool.cpp
)

set(VDB_STORAGE_SOURCES
    src/storage/mmap_file.cpp
    src/storage/io_prefetch.cpp
    src/storage/block_cache.cpp
    src/storage/chunk_cache.cpp
)

set(VDB_DIST_SOURCES
    src/dist/distance.cpp
    src/dist/distance_avx2.cpp
    src/dist/distance_avx512.cpp
)

set(VDB_INDEX_SOURCES
    src/index/ivf.cpp
    src/index/pq.cpp
    src/index/segment.cpp
    src/index/catalog.cpp
)

set(VDB_COLUMNS_SOURCES
    src/columns/column_store.cpp
    src/columns/column_chunk_reader.cpp
    src/columns/column_chunk_writer.cpp
    src/columns/encoders/raw.cpp
    src/columns/encoders/bitpack.cpp
    src/columns/encoders/dict.cpp
    src/columns/encoders/rle.cpp
    src/columns/encoders/varint.cpp
    src/columns/indexes/offset_index.cpp
    src/columns/indexes/zonemap.cpp
    src/columns/indexes/bloom.cpp
)

set(VDB_QUERY_SOURCES
    src/query/topk_heap.cpp
    src/query/scanner_ivf.cpp
    src/query/prefetch_policy.cpp
    src/query/query_engine.cpp
)

set(VDB_PAYLOAD_SOURCES
    src/payload/payload_api.cpp
)

# =============================================================================
# Main Library
# =============================================================================
add_library(vectordb STATIC
    ${VDB_COMMON_SOURCES}
    ${VDB_STORAGE_SOURCES}
    ${VDB_DIST_SOURCES}
    ${VDB_INDEX_SOURCES}
    ${VDB_COLUMNS_SOURCES}
    ${VDB_QUERY_SOURCES}
    ${VDB_PAYLOAD_SOURCES}
)

add_dependencies(vectordb flatbuffers_generated)

target_link_libraries(vectordb
    PUBLIC
        Threads::Threads
)

target_include_directories(vectordb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# =============================================================================
# Tools
# =============================================================================
if(VDB_BUILD_TOOLS)
    add_executable(vdb_ingest tools/ingest_main.cpp)
    target_link_libraries(vdb_ingest PRIVATE vectordb)
    
    add_executable(vdb_build_index tools/build_index_main.cpp)
    target_link_libraries(vdb_build_index PRIVATE vectordb)
    
    add_executable(vdb_query tools/query_main.cpp)
    target_link_libraries(vdb_query PRIVATE vectordb)
    
    add_executable(vdb_dump_meta tools/dump_meta_main.cpp)
    target_link_libraries(vdb_dump_meta PRIVATE vectordb)
endif()

# =============================================================================
# Tests
# =============================================================================
if(VDB_BUILD_TESTS)
    enable_testing()
    
    # Fetch Google Test
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.1
    )
    FetchContent_MakeAvailable(googletest)
    
    # Unit tests - Common
    add_executable(test_common tests/test_common.cpp)
    target_link_libraries(test_common PRIVATE vectordb GTest::gtest GTest::gtest_main)
    add_test(NAME test_common COMMAND test_common)
    
    # Unit tests - TopK Heap
    add_executable(test_topk_heap tests/test_topk_heap.cpp)
    target_link_libraries(test_topk_heap PRIVATE vectordb GTest::gtest GTest::gtest_main)
    add_test(NAME test_topk_heap COMMAND test_topk_heap)
    
    # Unit tests - Distance
    add_executable(test_distance tests/test_distance.cpp)
    target_link_libraries(test_distance PRIVATE vectordb GTest::gtest GTest::gtest_main)
    add_test(NAME test_distance COMMAND test_distance)
    
    # Unit tests - Column Store
    add_executable(test_column_store tests/test_column_store.cpp)
    target_link_libraries(test_column_store PRIVATE vectordb GTest::gtest GTest::gtest_main)
    add_test(NAME test_column_store COMMAND test_column_store)
    
    # Unit tests - Encoders
    add_executable(test_encoders tests/test_encoders.cpp)
    target_link_libraries(test_encoders PRIVATE vectordb GTest::gtest GTest::gtest_main)
    add_test(NAME test_encoders COMMAND test_encoders)
    
    # Integration tests - IVF-PQ
    add_executable(test_ivfpq_integration tests/test_ivfpq_integration.cpp)
    target_link_libraries(test_ivfpq_integration PRIVATE vectordb GTest::gtest GTest::gtest_main)
    add_test(NAME test_ivfpq_integration COMMAND test_ivfpq_integration)
endif()

# =============================================================================
# Benchmarks
# =============================================================================
if(VDB_BUILD_BENCHMARKS)
    add_executable(vdb_benchmark benchmarks/bench_main.cpp)
    target_link_libraries(vdb_benchmark PRIVATE vectordb)
    
    set_target_properties(vdb_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/benchmarks
    )
endif()

# =============================================================================
# Installation
# =============================================================================
install(TARGETS vectordb
    EXPORT vectordb-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/vdb
    DESTINATION include
)

install(EXPORT vectordb-targets
    FILE vectordb-config.cmake
    NAMESPACE vdb::
    DESTINATION lib/cmake/vectordb
)

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
