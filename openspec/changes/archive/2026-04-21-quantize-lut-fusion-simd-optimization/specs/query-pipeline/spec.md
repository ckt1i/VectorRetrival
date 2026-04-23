## ADDED Requirements

### Requirement: Query pipeline SHALL treat survivor compaction and batch classify as formal SIMD boundaries
resident 查询路径在完成 block-driven submit 之后，系统 MUST 将 Stage1 survivor compaction 和 batch classify 视为正式的下一轮 SIMD 边界。后续优化 MAY 保持结果语义不变，但 MUST 允许从 SafeOut mask 到 survivor index、再到 SafeIn/Uncertain 分类的批量处理，而不继续把 `mask` 之后的主路径固定为逐 lane 控制流。

#### Scenario: Survivor compaction is a formal optimization boundary
- **WHEN** 一个 FastScan block 产生多个非-SafeOut 候选
- **THEN** 系统 MUST 允许批量提取 survivor index
- **AND** 该边界 MUST 可作为独立优化点继续演进

#### Scenario: Batch classify preserves result semantics
- **WHEN** Stage1 采用 batch classify 处理 survivor
- **THEN** recall、最终排序和 SafeIn/Uncertain 语义 MUST 与参考路径保持一致

### Requirement: Query pipeline SHALL expose address batch decode and submit-prep as follow-up SIMD candidates
在 resident 查询路径下，`DecodeAddressBatch` 与 batch submit 前整理 SHALL 被视为正式的后续 SIMD / layout 优化点。实现 MAY 暂时保留标量内部逻辑，但系统 MUST 不把这两段重新隐藏回逐候选提交流程中。

#### Scenario: Address batch decode remains batch-first
- **WHEN** resident 路径处理一个 candidate batch
- **THEN** 地址解码 MUST 保持批量接口和批量语义

#### Scenario: Submit preparation remains batch-first
- **WHEN** resident 路径准备一次 batch submit
- **THEN** slot 分配、class 分组或后续整理 MUST 可以按 batch 粒度继续优化
