# Spec: Query Pipeline

## Feature: AsyncReader

### Scenario: PreadFallbackReader 同步读取
- Given 一个已打开的 DataFile（fd 有效）
- When 提交一个 IoRequest{fd, offset=0, length=512, buffer}
- Then Submit 返回 1（成功提交 1 个）
- And Poll 返回 1 个 IoCompletion{result=512}
- And buffer 中包含文件偏移 0 处的 512 字节数据
- And InFlight() 返回 0（同步完成）

### Scenario: PreadFallbackReader 批量提交
- Given 一个包含 100 条记录的 DataFile
- When 提交 10 个 IoRequest（不同 offset）
- Then Submit 返回 10
- And Poll 返回 10 个 IoCompletion，result 均为正数
- And 每个 buffer 包含对应 offset 的正确数据

### Scenario: PreadFallbackReader 读取超出文件范围
- Given 一个大小为 1000 字节的文件
- When 提交 IoRequest{offset=900, length=200}
- Then IoCompletion.result = 100（只读到文件末尾）

### Scenario: IoUringReader 基本读写（需 liburing）
- Given io_uring 可用（内核 >= 5.1）
- And 一个已打开的 DataFile
- When Init(queue_depth=32)
- And Submit 一个 IoRequest
- And WaitAndPoll
- Then 返回 1 个 IoCompletion{result > 0}
- And buffer 包含正确数据

### Scenario: IoUringReader SQ 满时部分提交
- Given Init(queue_depth=4)
- And 已有 4 个 in-flight 请求
- When Submit 2 个新请求
- Then Submit 返回 0（SQ 满）
- And InFlight() == 4

---

## Feature: ReadQueue

### Scenario: Qv 优先出队
- Given 空队列（max_capacity=256）
- When Push 3 个 priority=1 (Qp) 的 task
- And Push 2 个 priority=0 (Qv) 的 task
- And PopBatch(max_count=5)
- Then 前 2 个是 Qv task，后 3 个是 Qp task

### Scenario: 队列容量上限 back-pressure
- Given max_capacity=4 的队列
- When Push 4 个 task → 均返回 true
- And Push 第 5 个 task
- Then 返回 false（队列满）
- And Size() == 4

### Scenario: 空队列 PopBatch
- Given 空队列
- When PopBatch(max_count=10)
- Then 返回 0

### Scenario: Qv 全取完后取 Qp
- Given 队列中有 1 个 Qv task + 5 个 Qp task
- When PopBatch(max_count=3)
- Then 返回 3：1 个 Qv + 2 个 Qp

---

## Feature: ResultCollector

### Scenario: 基本 Top-K 收集
- Given top_k=3 的 ResultCollector
- When TryInsert(id=1, dist=5.0)
- And TryInsert(id=2, dist=3.0)
- And TryInsert(id=3, dist=7.0)
- Then Size() == 3, Full() == true
- And TopDistance() == 7.0

### Scenario: 替换堆顶
- Given top_k=3，已插入 dist=[5.0, 3.0, 7.0]
- When TryInsert(id=4, dist=4.0)
- Then 返回 true（4.0 < 7.0，替换成功）
- And TopDistance() == 5.0
- And Size() == 3

### Scenario: 拒绝过大距离
- Given top_k=3，已插入 dist=[5.0, 3.0, 4.0]
- When TryInsert(id=5, dist=6.0)
- Then 返回 false（6.0 > 5.0）
- And Size() == 3

### Scenario: Finalize 排序
- Given top_k=3，已插入 (id=1, 5.0), (id=2, 3.0), (id=3, 4.0)
- When Finalize()
- Then 返回 [(id=2, 3.0), (id=3, 4.0), (id=1, 5.0)]（distance 升序）

### Scenario: 堆未满时全部接受
- Given top_k=5 的空 ResultCollector
- When TryInsert 3 次
- Then 全部返回 true
- And Full() == false

---

## Feature: RerankConsumer

### Scenario: VEC_ONLY 消费 — 进入 TopK
- Given query_vec 和 dim=128 的 RerankConsumer
- And ResultCollector(top_k=10) 未满
- When Consume 一个 VEC_ONLY CompletedRead（buffer 含 128×float 向量）
- Then collector.Size() 增加 1
- And stats.total_reranked 增加 1

### Scenario: VEC_ONLY 消费 — 距离过大不进入 TopK
- Given collector 已满（10 条），TopDistance=2.0
- When Consume 一个 VEC_ONLY CompletedRead，其向量与 query 距离=5.0
- Then collector.Size() 仍为 10
- And TopDistance() 仍为 2.0

---

## Feature: OverlapScheduler

### Requirement: OverlapScheduler
在保持现有结果正确性和 recall 语义的前提下，`OverlapScheduler` SHALL 支持 resident 专用查询路径。当 `clu_read_mode=full_preload` 且 `use_resident_clusters=1` 时，系统 MUST 允许使用 resident query hot path、可复用 query wrapper 以及 resident 条件下的轻量候选提交组织方式。`coarse_select_ms` MUST 继续独立于 `probe_time_ms` 统计；`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms` MUST 继续作为 resident query 主路径的正式输出字段。

### Scenario: 端到端小规模搜索
- Given 一个已构建的 IVF 索引（N=1000, dim=128, nlist=4）
- And PreadFallbackReader
- When Search(query_vec, top_k=10, nprobe=2)
- Then 返回 10 个 SearchResult
- And 结果按 distance 升序排列
- And stats.total_probed > 0
- And stats.total_io_submitted > 0

### Scenario: 全部 SafeOut 时不发 I/O
- Given 一个索引，且 ConANN epsilon 极大（所有 candidate 都 SafeOut）
- When Search(query_vec, top_k=10, nprobe=1)
- Then 返回 0 个结果
- And stats.total_io_submitted == 0

### Scenario: nprobe=1 单 cluster
- Given 一个索引（nlist=4, cluster_0 有 100 条记录）
- When Search(query_vec, top_k=5, nprobe=1)
- Then 返回 min(5, non-SafeOut count) 个结果
- And 所有结果的 vec_id 解码后 cluster_id == 最近 cluster

### Scenario: Probe + I/O overlap
- Given 一个索引（nlist=8, 每 cluster 200 条）
- And probe_batch_size=64, io_batch_size=32
- When Search(query_vec, top_k=10, nprobe=4)
- Then 完成搜索（不死锁、不遗漏）
- And stats.total_probed == sum of non-SafeOut cluster sizes

### Scenario: Resident hot path exposes query-path timings
- **WHEN** 查询使用 `full_preload + use_resident_clusters=1`
- **THEN** 搜索统计 MUST 独立输出 `coarse_select_ms`
- **AND** MUST 独立输出 `probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms`
- **AND** `probe_time_ms` MUST 保持为这些 probe 子阶段的兼容聚合字段，而不包含 `coarse_select_ms`

### Scenario: Resident single-assignment serving may use lightweight submit organization
- **WHEN** resident 查询路径满足 single-assignment 条件
- **THEN** 系统 MAY 使用 resident 专用轻量候选提交组织方式
- **AND** 最终 recall 语义与结果排序 MUST 与参考路径保持一致

---

## Feature: ClusterStoreReader Codes 缓存

### Scenario: 全量缓存后 GetCodePtr 返回有效指针
- Given 一个已 Open 的 ClusterStoreReader
- When EnsureClusterLoaded(cluster_id=0)
- Then GetCodePtr(0, 0) 返回非空指针
- And GetCodePtr(0, num_records-1) 返回非空指针
- And 返回的 code 数据与逐条 LoadCode 一致

### Scenario: codes 缓存与 LoadCode 一致性
- Given 已 EnsureClusterLoaded 的 cluster
- When 对所有记录分别调用 GetCodePtr 和 LoadCode
- Then 两种方式返回的 code 数据完全一致

## ADDED Requirements (from resident-thinpath-batch-rerank-optimization)

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
- **THEN** 查询管线必须区分"probe 期间按 cluster 分批从磁盘预读到内存池"和"all-probe-done 后从内存池批量读取并执行 rerank"这两类原始向量读取

### Requirement: 查询管线必须输出稳定分段统计
resident thin path 和 batched rerank 引入后，查询管线 MUST 输出稳定、可解释的分段统计，至少包括 `probe_ms`、`prefetch_submit_ms`、`prefetch_wait_ms`、`safein_payload_prefetch_ms`、`candidate_collect_ms`、`pool_vector_read_ms`、`rerank_compute_ms`、`remaining_payload_fetch_ms`、`e2e_ms`、`num_candidates_buffered`、`num_candidates_reranked`、`num_safein_payload_prefetched`、`num_remaining_payload_fetches`。

#### Scenario: 查询结果包含分段统计
- **WHEN** 查询完成并输出 benchmark 结果
- **THEN** 结果中必须包含约定的分段时间和候选计数字段

## ADDED Requirements (from coarse-centroid-score-simd-optimization)

### Requirement: OverlapScheduler / query path SHALL allow coarse selection to use prebuilt packed centroid layouts
The query pipeline SHALL allow coarse cluster selection to consume a centroid layout prepared before query execution, so that the coarse score kernel can focus on computation rather than layout conversion.

#### Scenario: Packed coarse layout is used transparently during query
- **WHEN** `FindNearestClusters()` runs on an index that prepared a coarse packed layout during `Open()`
- **THEN** the query path SHALL use that packed layout for coarse scoring
- **AND** the rest of the query pipeline SHALL continue to receive the same ordered cluster IDs as before

#### Scenario: Query normalize and top-n selection remain semantically unchanged
- **WHEN** the coarse centroid score kernel is optimized
- **THEN** the query pipeline SHALL keep the existing query normalize behavior for cosine serving
- **AND** it SHALL keep the existing top-n selection semantics after score computation
