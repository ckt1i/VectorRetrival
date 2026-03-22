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
    ${FLATBUFFERS_SCHEMA_DIR}/record.fbs
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
# VDB Columns Library (Phase 2)
# =============================================================================
add_library(vdb_columns STATIC
    src/columns/column_chunk_writer.cpp
    src/columns/column_chunk_reader.cpp
    src/columns/column_store.cpp
)
target_include_directories(vdb_columns PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_columns PUBLIC vdb_common Threads::Threads)

# =============================================================================
# VDB SIMD Library (Phase 3)
# Only AVX2 + scalar paths are implemented. AVX-512 is deferred.
# See UNDO.txt [PHASE3-001] for the AVX-512 implementation plan.
# =============================================================================
add_library(vdb_simd STATIC
    src/simd/distance_l2.cpp
    src/simd/bit_unpack.cpp
    src/simd/prefix_sum.cpp
    src/simd/popcount.cpp
    src/simd/transpose.cpp
)
target_include_directories(vdb_simd PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_simd PUBLIC vdb_common)

# =============================================================================
# VDB Codec Library (Phase 3)
# =============================================================================
add_library(vdb_codec STATIC
    src/codec/bitpack_codec.cpp
    src/codec/dict_codec.cpp
)
target_include_directories(vdb_codec PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_codec PUBLIC vdb_simd)

# =============================================================================
# VDB RaBitQ Library (Phase 4)
# 1-bit RaBitQ quantization: rotation, encoder, estimator.
# See RabitQemplementation.md for algorithm details.
# =============================================================================
add_library(vdb_rabitq STATIC
    src/rabitq/rabitq_rotation.cpp
    src/rabitq/rabitq_encoder.cpp
    src/rabitq/rabitq_estimator.cpp
)
target_include_directories(vdb_rabitq PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_rabitq PUBLIC vdb_simd)

# =============================================================================
# VDB Storage Library (Phase 5)
# Provides AddressColumn, DataFileWriter, DataFileReader, ClusterStore, Segment.
# =============================================================================
add_library(vdb_storage STATIC
    src/storage/address_column.cpp
    src/storage/data_file_writer.cpp
    src/storage/data_file_reader.cpp
    src/storage/cluster_store.cpp
    src/storage/segment.cpp
)
target_include_directories(vdb_storage PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_storage PUBLIC vdb_columns vdb_codec vdb_rabitq)

# =============================================================================
# VDB Index Library (Phase 6)
# IVF index: ConANN 3-class classifier, IvfIndex, IvfBuilder.
# =============================================================================
add_library(vdb_index STATIC
    src/index/conann.cpp
    src/index/ivf_index.cpp
    src/index/ivf_builder.cpp
)
target_include_directories(vdb_index PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${FLATBUFFERS_GENERATED_DIR}>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_index PUBLIC vdb_storage vdb_rabitq vdb_simd vdb_schema)

# =============================================================================
# VDB Query Library (Phase 7)
# Single-threaded two-phase query pipeline: Probe+Submit → Drain+Rerank.
# =============================================================================
find_library(URING_LIB uring)
if(NOT URING_LIB)
    message(FATAL_ERROR "liburing not found. Install with: sudo apt install liburing-dev")
endif()

add_library(vdb_query STATIC
    src/common/distance.cpp
    src/query/pread_fallback_reader.cpp
    src/query/io_uring_reader.cpp
    src/query/result_collector.cpp
    src/query/rerank_consumer.cpp
    src/query/overlap_scheduler.cpp
    src/query/buffer_pool.cpp
)
target_include_directories(vdb_query PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_query PUBLIC vdb_index vdb_storage vdb_simd vdb_rabitq)
target_link_libraries(vdb_query PRIVATE ${URING_LIB})

# =============================================================================
# VDB IO Library (Phase 8)
# Npy/Jsonl file loaders for dataset ingestion.
# =============================================================================
add_library(vdb_io STATIC
    src/io/npy_reader.cpp
    src/io/jsonl_reader.cpp
)
target_include_directories(vdb_io PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_io PUBLIC vdb_common)

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
    
    add_executable(test_record tests/schema/record_test.cpp)
    target_link_libraries(test_record PRIVATE vdb_common vdb_schema GTest::gtest GTest::gtest_main)
    add_dependencies(test_record flatbuffers_generated)
    add_test(NAME test_record COMMAND test_record)
    
    # -------------------------------------------------------------------------
    # Column store tests (Phase 2)
    # -------------------------------------------------------------------------
    add_executable(test_column_chunk tests/columns/column_chunk_test.cpp)
    target_link_libraries(test_column_chunk PRIVATE vdb_columns GTest::gtest GTest::gtest_main)
    add_test(NAME test_column_chunk COMMAND test_column_chunk)
    
    add_executable(test_column_store tests/columns/column_store_test.cpp)
    target_link_libraries(test_column_store PRIVATE vdb_columns GTest::gtest GTest::gtest_main)
    add_test(NAME test_column_store COMMAND test_column_store)

    # -------------------------------------------------------------------------
    # SIMD tests (Phase 3)
    # -------------------------------------------------------------------------
    add_executable(test_distance_l2 tests/simd/distance_l2_test.cpp)
    target_link_libraries(test_distance_l2 PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_distance_l2 COMMAND test_distance_l2)

    add_executable(test_bit_unpack tests/simd/bit_unpack_test.cpp)
    target_link_libraries(test_bit_unpack PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_bit_unpack COMMAND test_bit_unpack)

    add_executable(test_prefix_sum tests/simd/prefix_sum_test.cpp)
    target_link_libraries(test_prefix_sum PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_prefix_sum COMMAND test_prefix_sum)

    add_executable(test_transpose tests/simd/transpose_test.cpp)
    target_link_libraries(test_transpose PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_transpose COMMAND test_transpose)

    # -------------------------------------------------------------------------
    # Codec tests (Phase 3)
    # -------------------------------------------------------------------------
    add_executable(test_bitpack_codec tests/codec/bitpack_codec_test.cpp)
    target_link_libraries(test_bitpack_codec PRIVATE vdb_codec GTest::gtest GTest::gtest_main)
    add_test(NAME test_bitpack_codec COMMAND test_bitpack_codec)

    add_executable(test_dict_codec tests/codec/dict_codec_test.cpp)
    target_link_libraries(test_dict_codec PRIVATE vdb_codec GTest::gtest GTest::gtest_main)
    add_test(NAME test_dict_codec COMMAND test_dict_codec)

    # -------------------------------------------------------------------------
    # SIMD popcount test (Phase 4 prerequisite)
    # -------------------------------------------------------------------------
    add_executable(test_popcount tests/simd/popcount_test.cpp)
    target_link_libraries(test_popcount PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_popcount COMMAND test_popcount)

    # -------------------------------------------------------------------------
    # RaBitQ tests (Phase 4)
    # -------------------------------------------------------------------------
    add_executable(test_rabitq_rotation tests/rabitq/rabitq_rotation_test.cpp)
    target_link_libraries(test_rabitq_rotation PRIVATE vdb_rabitq GTest::gtest GTest::gtest_main)
    add_test(NAME test_rabitq_rotation COMMAND test_rabitq_rotation)

    add_executable(test_rabitq_encoder tests/rabitq/rabitq_encoder_test.cpp)
    target_link_libraries(test_rabitq_encoder PRIVATE vdb_rabitq GTest::gtest GTest::gtest_main)
    add_test(NAME test_rabitq_encoder COMMAND test_rabitq_encoder)

    add_executable(test_rabitq_estimator tests/rabitq/rabitq_estimator_test.cpp)
    target_link_libraries(test_rabitq_estimator PRIVATE vdb_rabitq vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_rabitq_estimator COMMAND test_rabitq_estimator)

    # -------------------------------------------------------------------------
    # Storage tests (Phase 5)
    # -------------------------------------------------------------------------
    add_executable(test_address_column tests/storage/address_column_test.cpp)
    target_link_libraries(test_address_column PRIVATE vdb_storage GTest::gtest GTest::gtest_main)
    add_test(NAME test_address_column COMMAND test_address_column)

    add_executable(test_data_file tests/storage/data_file_test.cpp)
    target_link_libraries(test_data_file PRIVATE vdb_storage GTest::gtest GTest::gtest_main)
    add_test(NAME test_data_file COMMAND test_data_file)

    add_executable(test_cluster_store tests/storage/cluster_store_test.cpp)
    target_link_libraries(test_cluster_store PRIVATE vdb_storage GTest::gtest GTest::gtest_main)
    add_test(NAME test_cluster_store COMMAND test_cluster_store)

    add_executable(test_segment tests/storage/segment_test.cpp)
    target_link_libraries(test_segment PRIVATE vdb_storage GTest::gtest GTest::gtest_main)
    add_test(NAME test_segment COMMAND test_segment)

    # -------------------------------------------------------------------------
    # Index tests (Phase 6)
    # -------------------------------------------------------------------------
    add_executable(test_conann tests/index/conann_test.cpp)
    target_link_libraries(test_conann PRIVATE vdb_index GTest::gtest GTest::gtest_main)
    add_test(NAME test_conann COMMAND test_conann)

    add_executable(test_ivf_index tests/index/ivf_index_test.cpp)
    target_link_libraries(test_ivf_index PRIVATE vdb_index GTest::gtest GTest::gtest_main)
    add_test(NAME test_ivf_index COMMAND test_ivf_index)

    add_executable(test_ivf_builder tests/index/ivf_builder_test.cpp)
    target_link_libraries(test_ivf_builder PRIVATE vdb_index GTest::gtest GTest::gtest_main)
    add_test(NAME test_ivf_builder COMMAND test_ivf_builder)

    add_executable(test_payload_pipeline tests/index/payload_pipeline_test.cpp)
    target_link_libraries(test_payload_pipeline PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_payload_pipeline COMMAND test_payload_pipeline)

    # ----- Benchmark (Phase 6) -----
    add_executable(bench_ivf_quality tests/index/bench_ivf_quality.cpp)
    target_link_libraries(bench_ivf_quality PRIVATE vdb_index)

    # ----- E2E Benchmark -----
    add_executable(bench_e2e tests/index/bench_e2e.cpp)
    target_link_libraries(bench_e2e PRIVATE vdb_query vdb_io)

    # -------------------------------------------------------------------------
    # Query Pipeline tests (Phase 7)
    # -------------------------------------------------------------------------
    add_executable(test_distance tests/common/distance_test.cpp)
    target_link_libraries(test_distance PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_distance COMMAND test_distance)

    add_executable(test_pread_fallback_reader tests/query/pread_fallback_reader_test.cpp)
    target_link_libraries(test_pread_fallback_reader PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_pread_fallback_reader COMMAND test_pread_fallback_reader)

    add_executable(test_io_uring_reader tests/query/io_uring_reader_test.cpp)
    target_link_libraries(test_io_uring_reader PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_io_uring_reader COMMAND test_io_uring_reader)

    add_executable(test_result_collector tests/query/result_collector_test.cpp)
    target_link_libraries(test_result_collector PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_result_collector COMMAND test_result_collector)

    add_executable(test_buffer_pool tests/query/buffer_pool_test.cpp)
    target_link_libraries(test_buffer_pool PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_buffer_pool COMMAND test_buffer_pool)

    add_executable(test_rerank_consumer tests/query/rerank_consumer_test.cpp)
    target_link_libraries(test_rerank_consumer PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_rerank_consumer COMMAND test_rerank_consumer)

    add_executable(test_overlap_scheduler tests/query/overlap_scheduler_test.cpp)
    target_link_libraries(test_overlap_scheduler PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_overlap_scheduler COMMAND test_overlap_scheduler)

    add_executable(test_early_stop tests/query/early_stop_test.cpp)
    target_link_libraries(test_early_stop PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_early_stop COMMAND test_early_stop)

    # -------------------------------------------------------------------------
    # IO tests (Phase 8)
    # -------------------------------------------------------------------------
    add_executable(test_npy_reader tests/io/npy_reader_test.cpp)
    target_link_libraries(test_npy_reader PRIVATE vdb_io GTest::gtest GTest::gtest_main)
    add_test(NAME test_npy_reader COMMAND test_npy_reader)

    add_executable(test_jsonl_reader tests/io/jsonl_reader_test.cpp)
    target_link_libraries(test_jsonl_reader PRIVATE vdb_io GTest::gtest GTest::gtest_main)
    add_test(NAME test_jsonl_reader COMMAND test_jsonl_reader)
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
