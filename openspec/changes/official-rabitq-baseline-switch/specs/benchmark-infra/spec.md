## MODIFIED Requirements

### Requirement: Benchmark 基础设施支持独立诊断输出
Benchmark 基础设施 SHALL 在 serving benchmark 之外支持独立的诊断运行，且诊断输出 SHALL 导出足够的结构化元数据，以支撑 builder、gate 以及 canonical baseline identity 的比较。

#### Scenario: 诊断 benchmark 输出包含 builder 与 gate 元数据
- **WHEN** 某次诊断运行完成并写出结果
- **THEN** 输出 SHALL 包含 dataset、builder identity、single/phase gate 状态、target metric family 以及 diagnostic run role 等结构化元数据

#### Scenario: Benchmark 聚合按 builder 和 gate 分组
- **WHEN** 生成了跨 `hierarchical_superkmeans`、`superkmeans` 与 `faiss_kmeans` 的多组 parity 输出
- **THEN** 基础设施 SHALL 支持在无需手工重命名结果的情况下完成按 builder identity 与阶段 gate 的 summary 聚合

#### Scenario: threshold summary 暴露目标 probe 层级
- **WHEN** 某个 builder parity 运行包含 target-threshold 聚合
- **THEN** summary 输出 SHALL 包含达到预设目标 `recall@10` 或 candidate-recall target 所需的最小 `nprobe`

#### Scenario: 导出 Faiss builder provenance
- **WHEN** benchmark 或 build 输出涉及 `coarse_builder=faiss_kmeans`
- **THEN** 结构化元数据 SHALL 额外记录 clustering source、effective metric 和 Faiss training configuration
- **AND** 这些字段 SHALL 足以区分“C++ 进程内 Faiss 训练”与“导入预计算 Faiss artifacts”两种运行口径

#### Scenario: 导出 canonical baseline identity
- **WHEN** benchmark 结果涉及 `IVF+RQ+rerank` 主比较 baseline
- **THEN** 结构化元数据 SHALL 显式记录 compressed-search kernel identity
- **AND** 该字段 SHALL 足以区分 `official_rabitq_library` 与 `faiss_ivfrq_reference`

### Requirement: Benchmark summary SHALL report target-threshold operating points
Benchmark 基础设施 SHALL 支持为 single coarse builder parity 运行以及 canonical baseline operating-point 运行导出达到目标 `recall@10` 或等价 candidate-recall 指标所需的最小 `nprobe` 与 candidate-budget。

#### Scenario: 输出达到目标 recall 所需最小 nprobe
- **WHEN** 一组 single coarse builder parity 运行覆盖了多个 `nprobe`
- **THEN** summary 输出 SHALL 包含达到预设目标 `recall@10` 或 candidate-recall target 所需的最小 `nprobe`

#### Scenario: 无法达到目标 threshold 时显式记录
- **WHEN** 所有已运行的 `nprobe` 都未达到预设 target threshold
- **THEN** summary 输出 SHALL 将该 builder / 阶段结果标记为 unreached，而不是静默省略

#### Scenario: canonical 官方 baseline summary 记录调优后的 candidate budget
- **WHEN** 官方 RabitQ baseline 为达到目标 `recall@10` 执行了 candidate-budget 调优
- **THEN** summary 输出 SHALL 同时记录所用 `nprobe`、candidate-budget、recall 以及 payload-inclusive latency
