# =============================================================================
# Benchmarks — controlled by VDB_BUILD_BENCHMARKS
# =============================================================================
if(NOT VDB_BUILD_BENCHMARKS)
    return()
endif()

# ----- IVF Quality Benchmark -----
add_executable(bench_ivf_quality bench_ivf_quality.cpp)
target_link_libraries(bench_ivf_quality PRIVATE vdb_index)

# ----- E2E Benchmark -----
add_executable(bench_e2e bench_e2e.cpp)
target_link_libraries(bench_e2e PRIVATE vdb_query vdb_io vdb_crc)

# ----- RaBitQ Accuracy Benchmark -----
add_executable(bench_rabitq_accuracy bench_rabitq_accuracy.cpp)
target_link_libraries(bench_rabitq_accuracy PRIVATE vdb_index vdb_io)

# ----- ConANN Recall Benchmark -----
add_executable(bench_conann_recall bench_conann_recall.cpp)
target_link_libraries(bench_conann_recall PRIVATE vdb_index vdb_io)

# ----- ConANN CRC Benchmark -----
add_executable(bench_conann_crc bench_conann_crc.cpp)
target_link_libraries(bench_conann_crc PRIVATE vdb_crc vdb_index vdb_io)

# ----- CRC Overlap Integration Benchmark -----
add_executable(bench_crc_overlap bench_crc_overlap.cpp)
target_link_libraries(bench_crc_overlap PRIVATE vdb_crc vdb_index vdb_io)

# ----- RaBitQ Diagnostic Benchmark -----
add_executable(bench_rabitq_diagnostic bench_rabitq_diagnostic.cpp)
target_link_libraries(bench_rabitq_diagnostic PRIVATE vdb_index vdb_io)

# ----- Vector Search Benchmark -----
add_executable(bench_vector_search bench_vector_search.cpp)
target_link_libraries(bench_vector_search PRIVATE vdb_crc vdb_index vdb_io)
if(VDB_USE_MKL)
    target_link_libraries(bench_vector_search PRIVATE mkl)
endif()
if(OpenMP_CXX_FOUND)
    target_link_libraries(bench_vector_search PRIVATE OpenMP::OpenMP_CXX)
endif()

# ----- Vector Search Benchmark (frozen inline baseline) -----
# Baseline version preserved for comparison against the library-path refactor.
# Change: vector-search-lib-unification (Phase 0.5).
# This target is frozen at the state of three completed optimizations:
#   encoder-mbit-fast-quantize + eps-calibration-simd + calibration-omp-parallel
# It is NOT actively maintained; if library API changes break it, apply only
# the minimal fix required to restore compilation, or mark as deprecated.
add_executable(bench_vector_search_inline bench_vector_search_inline.cpp)
target_link_libraries(bench_vector_search_inline PRIVATE vdb_crc vdb_index vdb_io)
if(VDB_USE_MKL)
    target_link_libraries(bench_vector_search_inline PRIVATE mkl)
endif()
if(OpenMP_CXX_FOUND)
    target_link_libraries(bench_vector_search_inline PRIVATE OpenMP::OpenMP_CXX)
endif()
