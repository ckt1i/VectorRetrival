# Spec: Benchmark Infrastructure

## 目录结构

- Benchmark 源文件位于 `benchmarks/` 顶层目录
- Unit test 源文件保留在 `tests/` 目录，不受影响

## 编译隔离

- `VDB_BUILD_BENCHMARKS=ON` 编译所有 benchmark targets
- `VDB_BUILD_BENCHMARKS=OFF`（默认）不编译任何 benchmark
- `VDB_BUILD_TESTS` 和 `VDB_BUILD_BENCHMARKS` 互相独立，可任意组合
- 支持 `cmake --build build --target <bench_name>` 单独编译指定 benchmark

## benchmarks/CMakeLists.txt

- 受 `VDB_BUILD_BENCHMARKS` 控制
- 每个 benchmark 独立 `add_executable` + `target_link_libraries`
- 不注册 `add_test()`（benchmark 不参与 ctest）

## 根 CMakeLists.txt 改动

- `VDB_BUILD_TESTS` 块内不再包含 bench_* targets
- 在 tests 块之后添加 `add_subdirectory(benchmarks)`

## Additional Requirements (from coarse-builder-and-cover-diagnostics)

### Requirement: Benchmark 基础设施支持独立诊断输出
Benchmark 基础设施 SHALL 在 serving benchmark 之外支持独立的诊断运行，且诊断输出 SHALL 导出足够的结构化元数据，以支撑 builder 和 assignment-aware 的比较。

#### Scenario: 诊断 benchmark 输出包含 builder 元数据
- **WHEN** benchmark 以 diagnostic mode 运行
- **THEN** 输出 SHALL 包含 dataset、builder、assignment mode 以及 diagnostic metric family 等结构化元数据

#### Scenario: 诊断结果可以被聚合
- **WHEN** 生成了跨 builder 或 assignment mode 的多组诊断 benchmark 输出
- **THEN** 基础设施 SHALL 支持在无需手工重命名结果的情况下完成 summary 聚合

## Additional Requirements (from faiss-cpp-coarse-builder-integration)

### Requirement: Benchmark infrastructure records diagnostic metadata for coarse-builder runs
Benchmark 基础设施 SHALL 在 serving benchmark 之外支持独立的诊断运行，且诊断输出 SHALL 导出足够的结构化元数据，以支撑 builder 和阶段 gate-aware 的比较。

#### Scenario: 诊断 benchmark 输出包含 builder 与 gate 元数据
- **WHEN** 某次诊断运行完成并写出结果
- **THEN** 输出 SHALL 包含 dataset、builder identity、single/phase gate 状态、target metric family 以及 diagnostic run role 等结构化元数据

#### Scenario: Benchmark aggregation groups parity outputs by builder and gate
- **WHEN** 生成了跨 `hierarchical_superkmeans`、`superkmeans` 与 `faiss_kmeans` 的多组 parity 输出
- **THEN** 基础设施 SHALL 支持在无需手工重命名结果的情况下完成按 builder identity 与阶段 gate 的 summary 聚合

#### Scenario: Threshold summaries expose target probe levels
- **WHEN** 某个 builder parity 运行包含 target-threshold 聚合
- **THEN** summary 输出 SHALL 包含达到预设目标 `recall@10` 或 candidate-recall target 所需的最小 `nprobe`

#### Scenario: Faiss builder provenance is exported
- **WHEN** benchmark 或 build 输出涉及 `coarse_builder=faiss_kmeans`
- **THEN** 结构化元数据 SHALL 额外记录 clustering source、effective metric 和 Faiss training configuration
- **AND** 这些字段 SHALL 足以区分"C++ 进程内 Faiss 训练"与"导入预计算 Faiss artifacts"两种运行口径

## Additional Requirements (from single-coarse-builder-parity-optimization)

### Requirement: Benchmark 基础设施支持独立诊断输出
Benchmark 基础设施 SHALL 在 serving benchmark 之外支持独立的诊断运行，且诊断输出 SHALL 导出足够的结构化元数据，以支撑 builder 和阶段 gate-aware 的比较。

#### Scenario: 诊断 benchmark 输出包含 builder 与 gate 元数据
- **WHEN** benchmark 以 single coarse builder parity diagnostic mode 运行
- **THEN** 输出 SHALL 包含 dataset、builder identity、single/phase gate 状态、target metric family 以及 diagnostic run role 等结构化元数据

#### Scenario: 诊断结果可以按 builder 和阶段聚合
- **WHEN** 生成了跨 `hierarchical_superkmeans`、`superkmeans` 与 `faiss_kmeans` 的多组 parity 输出
- **THEN** 基础设施 SHALL 支持在无需手工重命名结果的情况下完成按 builder identity 与阶段 gate 的 summary 聚合

### Requirement: Benchmark summary SHALL report target-threshold operating points
Benchmark 基础设施 SHALL 支持为 single coarse builder parity 运行导出达到目标 `recall@10` 或等价 candidate-recall 指标所需的最小 `nprobe`。

#### Scenario: 输出达到目标 recall 所需最小 nprobe
- **WHEN** 一组 single coarse builder parity 运行覆盖了多个 `nprobe`
- **THEN** summary 输出 SHALL 包含达到预设目标 `recall@10` 或 candidate-recall target 所需的最小 `nprobe`

#### Scenario: 无法达到目标 threshold 时显式记录
- **WHEN** 所有已运行的 `nprobe` 都未达到预设 target threshold
- **THEN** summary 输出 SHALL 将该 builder / 阶段结果标记为 unreached，而不是静默省略
