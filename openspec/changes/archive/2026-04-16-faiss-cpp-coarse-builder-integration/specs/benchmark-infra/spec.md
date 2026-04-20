## MODIFIED Requirements

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
- **AND** 这些字段 SHALL 足以区分“C++ 进程内 Faiss 训练”与“导入预计算 Faiss artifacts”两种运行口径
