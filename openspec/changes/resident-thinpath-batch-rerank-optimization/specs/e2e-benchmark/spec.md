## ADDED Requirements

### Requirement: 在线 E2E benchmark 不得执行 CRC calibration
`bench_e2e` 的在线 benchmark 运行 MUST 不将 CRC calibration 作为执行流程的一部分。在线 benchmark SHALL 直接加载预先生成的 CRC 参数或等价工件；若工件缺失，系统必须以明确错误或明确的非在线口径退出，而不是在同一轮在线 benchmark 中重新 calibration。

#### Scenario: CRC 工件存在时直接加载
- **WHEN** benchmark 运行且索引目录中存在 CRC 参数或等价工件
- **THEN** 系统必须直接加载这些工件并进入在线查询阶段

#### Scenario: CRC 工件缺失时不回退到在线 calibration
- **WHEN** benchmark 运行且缺少所需 CRC 工件
- **THEN** 系统不得在该次在线 benchmark 中执行 CRC calibration 作为隐式回退路径

### Requirement: 分阶段优化必须产出固定口径测速与 perf 结果
本次 resident thin path 与 batched rerank 优化实现 MUST 在第二阶段和第三阶段分别使用相同参数执行 benchmark 测速，并对各阶段结果执行 perf 采样，作为后续优化参考。

#### Scenario: 第二阶段输出测速与 perf
- **WHEN** 第二阶段 resident thin path 实现完成
- **THEN** 系统必须使用约定 benchmark 参数输出速度结果，并产出对应 perf 热点结果

#### Scenario: 第三阶段输出测速与 perf
- **WHEN** 第三阶段 batched rerank 实现完成
- **THEN** 系统必须使用与第二阶段一致的 benchmark 参数再次输出速度结果，并产出对应 perf 热点结果

### Requirement: E2E benchmark 必须输出新分段统计字段
在线 E2E benchmark MUST 输出至少以下字段：`probe_ms`、`prefetch_submit_ms`、`prefetch_wait_ms`、`safein_payload_prefetch_ms`、`candidate_collect_ms`、`pool_vector_read_ms`、`rerank_compute_ms`、`remaining_payload_fetch_ms`、`e2e_ms`、`num_candidates_buffered`、`num_candidates_reranked`、`num_safein_payload_prefetched`、`num_remaining_payload_fetches`。

#### Scenario: benchmark 输出新统计字段
- **WHEN** benchmark 完成一次查询轮次并写出结果
- **THEN** 结果中必须包含约定的新分段时间和候选计数字段

#### Scenario: benchmark 区分两类原始向量读取口径
- **WHEN** benchmark 输出 `prefetch_*` 与 `pool_vector_read_ms` 等字段
- **THEN** 结果必须能够区分 probe 阶段按 cluster 从磁盘预读原始向量到内存池的开销与 all-probe-done 后从内存池批量读取原始向量用于 rerank 的开销
