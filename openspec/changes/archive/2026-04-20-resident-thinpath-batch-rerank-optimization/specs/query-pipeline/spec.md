## ADDED Requirements

### Requirement: Resident 查询管线必须支持专用 thin path
查询管线 MUST 支持在 resident full-preload 模式下切换到专用 thin path，并在该路径中仅保留 query 相关的准备、阈值判断、probe、候选输出、rerank 和最终 payload 获取。

#### Scenario: Resident 查询走专用管线
- **WHEN** 查询运行于 resident full-preload 模式
- **THEN** 查询管线必须走 resident 专用 thin path

### Requirement: 查询管线必须支持候选收集后批量 rerank
查询管线 MUST 支持 probe 阶段先输出候选集合，再由后段统一执行原始向量读取、批量距离计算和 top-k 合并，而不是要求每个候选到达后立即完成 rerank。

#### Scenario: 候选先收集后统一处理
- **WHEN** probe 阶段结束一个 cluster 或完成全部目标 cluster 的 probe
- **THEN** 查询管线必须能够对已收集的候选执行批量 rerank 处理

### Requirement: 查询管线优化不得改变 recall 计算口径
resident thin path 和 batched rerank 引入后，查询管线 MUST 保持与优化前一致的 recall 计算口径和结果定义。

#### Scenario: Recall 口径保持不变
- **WHEN** 使用同一数据集、索引和 benchmark 参数比较优化前后结果
- **THEN** 系统必须按相同 recall 定义进行统计和输出

### Requirement: 查询管线必须支持沿用现有提交路径的全 probe 后统一 batch rerank
查询管线 MUST 支持 probe 阶段继续沿用现有 SafeIn 和 Uncertain 各自的提交与解码方式，并在全部目标 cluster 处理完成后，由后段统一执行候选整理、内存池读取、批量距离计算和 top-k 合并，而不是要求每个候选到达后立即完成 rerank。

#### Scenario: 全 probe 后统一处理
- **WHEN** probe 阶段完成全部目标 cluster 的处理
- **THEN** 查询管线必须能够对已收集候选执行统一的 batch rerank 处理

#### Scenario: 查询管线区分两类原始向量读取
- **WHEN** 查询进入 probe 阶段和后续 batch rerank 阶段
- **THEN** 查询管线必须区分“probe 期间按 cluster 分批从磁盘预读到内存池”和“all-probe-done 后从内存池批量读取并执行 rerank”这两类原始向量读取

### Requirement: 查询管线必须输出稳定分段统计
resident thin path 和 batched rerank 引入后，查询管线 MUST 输出稳定、可解释的分段统计，至少包括 `probe_ms`、`prefetch_submit_ms`、`prefetch_wait_ms`、`safein_payload_prefetch_ms`、`candidate_collect_ms`、`pool_vector_read_ms`、`rerank_compute_ms`、`remaining_payload_fetch_ms`、`e2e_ms`、`num_candidates_buffered`、`num_candidates_reranked`、`num_safein_payload_prefetched`、`num_remaining_payload_fetches`。

#### Scenario: 查询结果包含分段统计
- **WHEN** 查询完成并输出 benchmark 结果
- **THEN** 结果中必须包含约定的分段时间和候选计数字段
