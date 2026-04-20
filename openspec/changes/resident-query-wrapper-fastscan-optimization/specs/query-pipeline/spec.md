## MODIFIED Requirements

### Requirement: OverlapScheduler

在保持现有结果正确性和 recall 语义的前提下，`OverlapScheduler` SHALL 支持 resident 专用查询路径。当 `clu_read_mode=full_preload` 且 `use_resident_clusters=1` 时，系统 MUST 允许使用 resident query hot path、可复用 query wrapper 以及 resident 条件下的轻量候选提交组织方式。`coarse_select_ms` MUST 继续独立于 `probe_time_ms` 统计；`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms` MUST 继续作为 resident query 主路径的正式输出字段。

#### Scenario: 端到端小规模搜索
- **WHEN** 一个已构建的 IVF 索引（N=1000, dim=128, nlist=4）在通用 serving 模式下调用 `Search(query_vec, top_k=10, nprobe=2)`
- **THEN** 系统返回 10 个 SearchResult
- **AND** 结果按 distance 升序排列
- **AND** `stats.total_probed > 0`
- **AND** `stats.total_io_submitted > 0`

#### Scenario: 全部 SafeOut 时不发 I/O
- **WHEN** 一个索引使用极大的 ConANN epsilon，使所有 candidate 都为 SafeOut，并执行 `Search(query_vec, top_k=10, nprobe=1)`
- **THEN** 返回 0 个结果
- **AND** `stats.total_io_submitted == 0`

#### Scenario: nprobe=1 单 cluster
- **WHEN** 一个索引（nlist=4, cluster_0 有 100 条记录）执行 `Search(query_vec, top_k=5, nprobe=1)`
- **THEN** 返回 `min(5, non-SafeOut count)` 个结果
- **AND** 所有结果的 `vec_id` 解码后 `cluster_id` 等于最近 cluster

#### Scenario: Probe + I/O overlap
- **WHEN** 一个索引（nlist=8, 每 cluster 200 条）在 `probe_batch_size=64`、`io_batch_size=32` 条件下执行 `Search(query_vec, top_k=10, nprobe=4)`
- **THEN** 搜索 MUST 完成且不死锁、不遗漏
- **AND** `stats.total_probed ==` 所有非-SafeOut 候选总数

#### Scenario: Resident hot path exposes query-path timings
- **WHEN** 查询使用 `full_preload + use_resident_clusters=1`
- **THEN** 搜索统计 MUST 独立输出 `coarse_select_ms`
- **AND** MUST 独立输出 `probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms`
- **AND** `probe_time_ms` MUST 保持为这些 probe 子阶段的兼容聚合字段，而不包含 `coarse_select_ms`

#### Scenario: Resident single-assignment serving may use lightweight submit organization
- **WHEN** resident 查询路径满足 single-assignment 条件
- **THEN** 系统 MAY 使用 resident 专用轻量候选提交组织方式
- **AND** 最终 recall 语义与结果排序 MUST 与参考路径保持一致
