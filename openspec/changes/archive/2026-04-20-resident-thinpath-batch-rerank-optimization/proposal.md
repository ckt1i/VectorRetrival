## Why

在 `full_preload + use_resident_clusters=1` 的配置下，`bench_e2e` 的在线查询速度仍明显慢于当前轻量化的 IVF-RaBitQ rerank baseline，即使聚类 I/O 已经基本被消除。现有差距主要来自 resident 模式查询路径过厚、到达即精排的 rerank 执行方式，以及 benchmark 生命周期中仍混有不应计入在线查询口径的准备阶段工作。

现在需要推进这项变更，是因为后续优化已经不再是单点微调，而是要先修正 benchmark 口径，再分阶段重构 resident 在线路径和 rerank 执行模型，并在每个阶段结束后保留速度与 perf 热点证据，作为后续继续优化的基线。

## What Changes

- 将 CRC calibration 从 `bench_e2e` 的在线评测口径中彻底剥离，要求在线 benchmark 运行时直接消费预计算的 CRC 参数或工件，而不是在执行期间重新校准。
- 为 `full_preload + use_resident_clusters=1` 引入专用的 resident thin path，将除 query 相关阈值判断外的所有可前移步骤统一移动到 prepare/init 阶段，缩短在线查询路径。
- 引入 batched rerank 执行模式，并明确区分两类原始向量读取阶段：第一类是 probe 过程中按 cluster 分批从磁盘提交并完成的 raw vector prefetch 预读；第二类是全部目标 cluster probe 完成后，从原始向量内存池批量读取这些已预取向量并执行 batch rerank。该模式替代当前候选一到即做单条 `L2Sqr` 的执行方式。
- 保持当前 recall 语义、结果顺序和最终输出兼容，不以牺牲结果正确性换取速度。
- 将测速与 perf 采样纳入实施要求：第二阶段实现完成后先按固定参数测速与 perf，第三阶段实现完成后再次进行同口径测速与 perf。

## Capabilities

### New Capabilities
- `resident-thin-query-path`：定义 resident 内存模式下的专用轻量查询路径，要求将 query 无关的 cluster 元信息、解析结果和运行期辅助结构前移到 prepare/init 阶段。
- `batched-rerank-pipeline`：定义 probe 阶段磁盘分批 prefetch 预读、all-probe-done 后内存池批量读取与 batch rerank、以及分阶段 top-k 合并的查询后段执行能力。

### Modified Capabilities
- `e2e-benchmark`：修改 benchmark 生命周期要求，明确 CRC calibration 不属于在线查询计时口径，并要求第二阶段与第三阶段都输出同参数速度结果和 perf 热点结果。
- `crc-calibration`：修改 CRC 的运行期要求，在线 E2E benchmark 必须使用预先生成的 CRC 参数/工件，将 calibration 视为离线或 prepare 阶段能力。
- `query-pipeline`：修改 resident 查询与 rerank 执行要求，允许 resident thin path 与 batched rerank 替代现有通用厚路径和到达即精排模式，同时保持 recall 语义不变。

## Impact

- 受影响代码将包括 `benchmarks/bench_e2e.cpp`、resident/preload 查询路径、调度器与 rerank/collector 相关代码，以及 CRC 参数加载与接入逻辑。
- benchmark 的执行方式和结果记录会发生变化，因为阶段性测速与 perf 采样将成为实现验收的一部分。
- 不计划引入外部接口破坏，但内部执行结构和 benchmark 口径会发生实质变化。

## Proposal Addendum

- probe 阶段继续沿用现有 SafeIn 和 Uncertain 各自的提交与解码方式，不在本次变更中重写这两条路径。
- 每 probe 完一个 cluster，系统按现有路径提交该 cluster 候选对应的 raw vector prefetch。
- 这里的 raw vector prefetch 指的是按 cluster 分批从磁盘向原始向量内存池发起的预读，不等同于最终 batch rerank 前从内存池读取向量。
- 对于被判定为 SafeIn 的少量候选，系统按现有路径提前提交对应 payload prefetch。
- 待全部目标 cluster probe 完成后，系统统一整理候选集合，并从原始向量内存池读取已预取好的原始向量执行一轮 batch rerank。
- 这里的内存池读取指的是从原始向量内存池批量读取已预取完成的向量，组织 batch 输入并执行统一 rerank，不再属于磁盘预读阶段。
- 最终结果阶段仅对未命中 SafeIn payload 预取的结果执行 remaining payload fetch。
- 结果统计需要显式补充以下字段：`probe_ms`、`prefetch_submit_ms`、`prefetch_wait_ms`、`safein_payload_prefetch_ms`、`candidate_collect_ms`、`pool_vector_read_ms`、`rerank_compute_ms`、`remaining_payload_fetch_ms`、`e2e_ms`、`num_candidates_buffered`、`num_candidates_reranked`、`num_safein_payload_prefetched`、`num_remaining_payload_fetches`。
