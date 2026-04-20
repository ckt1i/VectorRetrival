## MODIFIED Requirements

### Requirement: Benchmark 基础设施支持独立诊断输出
Benchmark 基础设施 SHALL 在 serving benchmark 之外支持独立的诊断运行，且诊断输出 SHALL 导出足够的结构化元数据，以支撑 builder 和 assignment-aware 的比较。

#### Scenario: 诊断 benchmark 输出包含 builder 元数据
- **WHEN** benchmark 以 diagnostic mode 运行
- **THEN** 输出 SHALL 包含 dataset、builder、assignment mode 以及 diagnostic metric family 等结构化元数据

#### Scenario: 诊断结果可以被聚合
- **WHEN** 生成了跨 builder 或 assignment mode 的多组诊断 benchmark 输出
- **THEN** 基础设施 SHALL 支持在无需手工重命名结果的情况下完成 summary 聚合
