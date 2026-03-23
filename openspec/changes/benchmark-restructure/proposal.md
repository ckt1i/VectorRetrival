## Why

Benchmark 文件（bench_e2e, bench_rabitq_accuracy, bench_ivf_quality）混在 `tests/index/` 目录下，和 unit test 共用 `VDB_BUILD_TESTS` 编译开关。每次修改 benchmark 代码都需要编译全部 35 个 unit test，且 `VDB_BUILD_BENCHMARKS` 选项已声明但未使用。此外 bench_e2e 的参数几乎全部硬编码，无法灵活调参。最后，需要新增一个 ConANN recall benchmark 来评估 IVF 分区 + d_k early stop 在精确距离下的 recall 天花板。

## What Changes

- 新建 `benchmarks/` 顶层目录，将 3 个 bench_*.cpp 从 `tests/index/` 移入
- 新建 `benchmarks/CMakeLists.txt`，受 `VDB_BUILD_BENCHMARKS` 控制，独立于 `VDB_BUILD_TESTS`
- 根 CMakeLists.txt: 删除 `VDB_BUILD_TESTS` 块内的 bench_* targets，添加 `add_subdirectory(benchmarks)`
- bench_e2e 参数化: 将硬编码的 build/search 参数（nlist, nprobe, topk, early_stop 等）改为 CLI 参数
- 新增 `bench_conann_recall.cpp`: 使用精确 L2 距离 + ConANN d_k early stop 测试 recall，评估 IVF 分区质量上限

## Capabilities

### New Capabilities
- `benchmark-infra`: benchmark 目录独立化、独立 CMakeLists、编译隔离
- `conann-recall-bench`: 精确距离 + ConANN early stop 的 recall benchmark

### Modified Capabilities
- `e2e-benchmark`: 输入参数从硬编码改为 CLI 参数，支持运行时灵活配置

## Impact

- **文件移动**: `tests/index/bench_*.cpp` → `benchmarks/bench_*.cpp`
- **CMakeLists.txt**: 根 CMakeLists 和新 benchmarks/CMakeLists.txt
- **编译流程**: `cmake -DVDB_BUILD_BENCHMARKS=ON -DVDB_BUILD_TESTS=OFF` 可只编译 benchmark
- **现有 unit tests**: 不受影响（仍在 tests/ 下，受 VDB_BUILD_TESTS 控制）
