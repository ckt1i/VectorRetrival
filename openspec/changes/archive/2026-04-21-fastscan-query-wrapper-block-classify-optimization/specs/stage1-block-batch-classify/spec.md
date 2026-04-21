## ADDED Requirements

### Requirement: Stage1 SHALL support block-driven candidate compaction
Stage1 查询路径 MUST 不再只依赖逐 lane 的发现与消费模式。在 FastScan block 粒度下，系统 SHALL 先根据 `EstimateDistanceFastScan` 与 `FastScanSafeOutMask` 结果生成 block-local survivor 集合，再将 survivor compact 成稳定的 candidate batch，供后续 classify、地址解码和提交路径复用。

#### Scenario: SafeOut filtering happens before candidate batch materialization
- **WHEN** 一个 FastScan block 完成距离估计
- **THEN** 系统 MUST 先应用 SafeOut 过滤
- **AND** 只有非-SafeOut 候选才允许进入 block-local candidate batch

#### Scenario: Block-local candidate batch keeps per-candidate metadata
- **WHEN** 一个 block 内存在非-SafeOut 候选
- **THEN** candidate batch MUST 保留至少 `global_idx`、`est_dist` 和最终分类结果
- **AND** 这些字段 MUST 足以支撑后续地址解码和 sink 提交，而不要求重新回到逐 lane 路径

### Requirement: Stage1 SHALL classify and submit candidates through batch-friendly interfaces
Stage1 的 classify 之后路径 MUST 支持 batch 消费接口。系统 SHALL 为 `ProbeResultSink` 提供 batch 级别的候选消费能力，并 SHALL 支持将同一 block 或同一紧凑批次中的候选统一交给 sink，而不是强制每个候选都走一次独立的 `OnCandidate` 主路径。

#### Scenario: Sink can consume a compact batch
- **WHEN** 一个 block 内生成了 compact candidate batch
- **THEN** sink MUST 能接收该 batch 并完成后续处理
- **AND** 结果语义 MUST 与逐候选消费路径一致

#### Scenario: Single-assignment resident path may use lightweight batch submit
- **WHEN** resident 查询满足 single-assignment 条件
- **THEN** batch sink MAY 采用轻量的 batch submit 组织方式
- **AND** 最终 recall 和结果排序 MUST 与参考路径保持一致

### Requirement: ParsedCluster SHALL expose batch address decode as a stable boundary
为了支撑 Stage1 的 block-driven candidate batch，`ParsedCluster` MUST 暴露 address batch decode 能力。系统 SHALL 支持针对一组 `vec_idx` 一次性解码对应地址，并将它作为正式批量边界，而不是将地址解码永久嵌在逐候选路径内部。

#### Scenario: Raw address layout can be decoded in batch
- **WHEN** cluster 使用 raw address layout 且同一个 block 产生多个候选
- **THEN** 系统 MUST 能以 batch 方式解码这些候选对应的地址
- **AND** batch decode 的输出 MUST 与逐个 `AddressAt()` 的结果一致

#### Scenario: Batch decode remains valid when decoded-address cache is used
- **WHEN** cluster 已经持有 decoded address cache
- **THEN** batch decode MUST 仍然返回与 cache 一致的地址结果
- **AND** 不得因切换 batch 接口而改变地址语义

### Requirement: Stage1 batch interfaces SHALL define the next SIMD-ready boundaries
这次 block-driven 重构完成后，系统 SHALL 明确以下路径已成为稳定的后续 SIMD 候选边界：`safein/safeout mask 生成`、candidate compaction、address batch decode、以及 batch submit 前的按 class 数据整理。实现 MAY 在本轮保留标量版本，但这些边界 SHALL 已经可以独立 profiling 和替换。

#### Scenario: Follow-up SIMD candidates are visible after batch refactor
- **WHEN** block-driven classify 实现完成并重新执行 clean perf
- **THEN** profiling MUST 能单独观察 candidate compaction、address batch decode 或 batch submit 前整理的成本
- **AND** 后续 SIMD 优化 SHALL 可以围绕这些边界展开，而不需要再次拆分主查询流程
