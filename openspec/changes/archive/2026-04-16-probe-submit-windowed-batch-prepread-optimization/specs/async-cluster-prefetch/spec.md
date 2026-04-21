## ADDED Requirements

### Requirement: Async prefetch pipeline SHALL support type-bucketed batch PrepRead
异步预取链路 MUST 支持在 query 在线路径中按请求类型对候选进行分桶，并以 batch 方式准备读取计划。系统 MUST 至少区分 `VEC_ONLY` 和 `ALL` 两类请求，并先完成同类请求的批量地址/长度/slot/buffer 准备，再统一发射 SQE。

#### Scenario: Vec-only requests are prepared in a dedicated batch
- **WHEN** 一个 cluster 产生多条 `VEC_ONLY` 候选
- **THEN** 系统必须将这些请求作为一个 `vec_only` 批次完成读取计划准备
- **AND** 不得要求每个候选单独走完整的 `PrepReadVecOnly` 准备路径

#### Scenario: All requests are prepared in a dedicated batch
- **WHEN** 一个 cluster 产生多条 `ALL` 候选
- **THEN** 系统必须将这些请求作为一个 `all` 批次完成读取计划准备
- **AND** 该批次必须继续沿用现有 `ALL` 路径所要求的 payload 可用语义

### Requirement: Async prefetch pipeline SHALL separate batch prepare from submit
异步预取链路 MUST 将“批量准备读取计划”和“触发 submit”作为两个独立阶段处理。`probe_submit_ms` MUST 覆盖用户态批量准备与 SQE 发射成本，而 `uring_submit_ms` MUST 继续仅表示提交动作本身的开销。

#### Scenario: Batch prepare completes before submit decision
- **WHEN** 一个 cluster 内的候选已经完成类型分桶
- **THEN** 系统必须先完成该批次的读取计划准备与 SQE 发射
- **AND** 再根据 submit window 规则决定是否立即提交

#### Scenario: Benchmark can attribute prepare and submit separately
- **WHEN** benchmark 导出查询分段统计
- **THEN** 输出必须能够区分批量 prepare 成本与 `io_uring_submit` 成本
- **AND** 不得把两者合并为单一不可解释字段
