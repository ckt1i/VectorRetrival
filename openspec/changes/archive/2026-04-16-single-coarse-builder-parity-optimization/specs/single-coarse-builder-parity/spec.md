## ADDED Requirements

### Requirement: Single coarse builder parity workflow SHALL execute phased builder gating
系统 SHALL 提供 single-assignment coarse builder parity 工作流，并按阶段执行 builder gate：先比较 `hierarchical_superkmeans` 与 `superkmeans`；仅当单层 SuperKMeans 仍不能显著缩小 high-recall probe gap 时，才进入 `faiss_kmeans` parity 阶段。

#### Scenario: Phase 1 runs current layered path against single-layer SuperKMeans
- **WHEN** 用户为 single coarse builder parity 发起第一阶段对照
- **THEN** 系统 SHALL 在相同数据集、metric、`nlist`、single assignment 与评估契约下运行 `hierarchical_superkmeans` 和 `superkmeans`

#### Scenario: Phase 2 is conditional on phase-1 failure
- **WHEN** 第一阶段结果未达到预设成功标准，或未达到与 baseline 对齐所需的 probe 水平
- **THEN** 系统 SHALL 进入 `faiss_kmeans` parity 阶段，而 SHALL NOT 要求手工重新定义新的 change scope

### Requirement: Phase 1 success criteria SHALL be explicitly recorded
系统 SHALL 为单层 SuperKMeans 对照记录显式成功 / 失败结论，并将该结论作为是否进入 Faiss parity 的 gate。

#### Scenario: Phase 1 writes gate outcome
- **WHEN** 第一阶段 builder parity 运行完成
- **THEN** 系统 SHALL 在结构化输出中记录 gate outcome、比较对象以及是否进入 phase 2

#### Scenario: Phase 1 may stop the workflow early
- **WHEN** 单层 SuperKMeans 已达到预设目标 probe threshold 或显著缩小与 baseline 的差距
- **THEN** 系统 SHALL 支持将该结果记录为 phase-1 pass，并允许 workflow 在不进入 Faiss parity 的情况下结束
