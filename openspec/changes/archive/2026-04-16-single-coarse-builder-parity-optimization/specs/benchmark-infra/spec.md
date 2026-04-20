## MODIFIED Requirements

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
