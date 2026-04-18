## ADDED Requirements

### Requirement: Pure coarse-cover 诊断必须隔离 coarse partition 质量
系统 SHALL 提供 pure coarse-cover 诊断模式，在 IVF+Flat 语义下测量 ground-truth top-k 邻居是否落入已探测 coarse cluster 集合；该模式不得依赖 CRC early stop、epsilon pruning 或 cluster 内 candidate pruning。

#### Scenario: coarse cover 在无 serving-side pruning 条件下评估
- **WHEN** 执行 pure coarse-cover 诊断运行
- **THEN** 输出的 cover 指标 SHALL 仅反映 cluster selection coverage，而 SHALL NOT 依赖 CRC、epsilon 或 candidate pruning 设置

#### Scenario: top-k cover 基于 ground truth 计算
- **WHEN** 对 `topk = 10` 执行 pure coarse-cover 诊断运行
- **THEN** 输出 SHALL 报告 GT top-10 邻居中，其所属 coarse cluster 落入已探测 cluster 集合的比例

### Requirement: assignment-mode 对照必须使用统一契约导出
系统 SHALL 在同一输出 schema 下支持 `single`、`redundant_top2_naive` 和 `redundant_top2_rair` 至少三种 assignment mode 的 pure coarse-cover 诊断。

#### Scenario: assignment mode 可直接比较
- **WHEN** 在同一数据集和 builder 下为多个 assignment mode 生成诊断运行
- **THEN** 输出 SHALL 在每一行中包含 assignment mode 字段，并允许直接比较其 cover-vs-probe 行为

### Requirement: 诊断输出必须包含派生的 probe 阈值
诊断工作流 SHALL 同时导出原始 cover-vs-probe 曲线，以及达到目标 cover 所需的最小 probe 阈值。

#### Scenario: 导出目标 cover 所需最小 nprobe
- **WHEN** 一次诊断运行在多个 probe 值上输出了 coarse cover
- **THEN** 输出 SHALL 包含达到 `0.95`、`0.99`、`0.995` 等目标 cover 所需的最小 `nprobe`

#### Scenario: 无法达到目标 cover 时显式记录
- **WHEN** 所有 probe 值都未达到某个配置的目标 cover
- **THEN** 输出 SHALL 将该目标标记为 unreached，而不是静默省略
