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
