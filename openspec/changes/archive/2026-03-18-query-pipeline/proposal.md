# Proposal: Query Pipeline (Phase 7)

## What

实现 VDB VectorRetrival 的查询流水线——从接收查询请求到返回 Top-K 结果的完整路径。采用单线程两阶段架构：Phase 1 全速 Probe 所有 cluster 并提交 I/O 到 io_uring；Phase 2 从 CQ 逐个读取完成结果并 L2Sqr 精排。核心组件包括：

1. **AsyncReader** — io_uring 异步 I/O 封装（`PrepRead` + `Submit` 映射 io_uring 原语）+ pread 同步 fallback（运行时降级）。CQE `user_data` 存储 buffer 指针，配合 `pending_` map 还原元信息
2. **ResultCollector** — Top-K max-heap 收集器（`CollectorEntry{distance, AddressEntry}` 为独立 struct，无 VecID）
3. **SearchContext** — 查询上下文，持有 query、config（含 safein_all_threshold=256KB、cq_entries=4096）、统计
4. **RerankConsumer** — Rerank 消费者 + payload_cache 管理。ALL buffer 在 ConsumeAll 中拆分（memcpy payload 到新 buffer），cache 统一 payload offset 0。被淘汰条目惰性释放
5. **OverlapScheduler** — 单线程两阶段调度：`ProbeAndSubmit` 全速 Probe + flush per cluster → `DrainAndRerank` 从 CQ 逐条精排。内部维护 `pending_: unordered_map<uint8_t*, PendingRead>`，通过 `InferType` 从读取参数推断 ReadTaskType
6. **SearchResults** — RAII 结果包装，自动管理 payload buffer 生命周期。`SearchResult{distance, Datum* payload}`，Datum 初期为单个值
7. **L2Sqr** — 精确 L2 距离计算（`common/distance.h`），薄包装委托 `simd::L2Sqr`（已有 AVX2 + 标量回退）
8. **BufferPool** — I/O buffer 分配/回收池

## Why

Phase 0–6 已完成全部离线构建和存储层。目前系统可以构建索引但无法执行查询。Phase 7 是将系统从"只写"变为"可查询"的关键一步。

关键设计目标：
- **单线程两阶段**：Phase 1 Probe+Submit → Phase 2 Drain+Rerank，无线程、无锁、无应用层队列
- **内核级 CPU/IO 重叠**：Phase 1 中每个 cluster probe 完毕后 `Submit` flush，内核立即开始处理 SQ 中的请求；Phase 2 开始 drain CQ 时大部分 I/O 已就绪
- **io_uring SQ/CQ 即队列**：无需 ReadQueue 或 SPSC queue，io_uring 的 SQ/CQ 即为唯一的任务队列和完成队列
- **SafeIn 全量预读**：高置信度记录（≤256KB）一次 ALL 读取 vec+payload，省一半 IOPS；>256KB 拆分为 VEC_ONLY + PAYLOAD
- **异步 I/O**：io_uring PrepRead+Submit，CQE user_data 存 buffer 指针 + pending_ map 还原元信息
- **RAII 结果管理**：SearchResults 自动管理 payload buffer 生命周期
- **类型推断**：从 PendingRead 的 read_offset/read_length 推断 ReadTaskType，无需在 CQE 中显式传递
- **CQ 大容量**：`IORING_SETUP_CQSIZE` 设 cq_entries=4096，防止 Phase 1 全部 Probe 完才 drain 导致 CQ 溢出

## Scope

### In Scope
- io_uring 异步读取（liburing 构建时必须，运行时 fallback 到 pread）
- AsyncReader 接口：`PrepRead()` + `Submit()` 映射 io_uring 原语
- CQE user_data = buffer 指针 + `pending_` map 元信息还原 + `InferType` 类型推断
- CQ 容量 4096（`IORING_SETUP_CQSIZE`），防止大 nprobe 场景溢出
- 3 种 ReadTaskType: VEC_ONLY / ALL / PAYLOAD（从 5 种精简）
- 清理 types.h：删除 FRONT/BACK、删除 `ReadTask` 和 `CompletedRead` 结构体（新模型不使用队列传递任务）、删除 `Candidate::vec_id`
- SafeIn ALL 全量读取（≤256KB 阈值，>256KB 拆分 VEC_ONLY+PAYLOAD）
- ALL buffer 拆分策略：ConsumeAll 中 memcpy payload 到新 buffer 入 cache，原始 buffer 始终释放
- Top-K 结果收集（CollectorEntry 为独立 struct，无 VecID，只返回 distance + Datum payload）
- RAII SearchResults 包装（move-only，析构自动释放 payload buffer）
- 单线程两阶段调度：ProbeAndSubmit（全速 Probe + flush per cluster）→ DrainAndRerank（从 CQ 逐条精排）
- Probe 粒度：聚类 ≤1024 原子 probe，>1024 分 64 条 mini-batch
- Phase 3 Uncertain payload 批量补读
- ClusterStoreReader codes 全量缓存（解决 Probe 阶段逐条 pread 瓶颈）
- DataFileReader 新增 `fd()` 访问器（OverlapScheduler 需要 fd 传给 PrepRead）
- L2Sqr 精确距离计算（common/distance.h，薄包装委托 simd::L2Sqr）
- BufferPool（简单 Acquire/Release 池）
- Datum 初期为单个值（非多列序列）
- 单元测试 + 端到端测试（ConANN 阈值初步硬编码）

### Out of Scope
- 多线程并发（Taskflow、SPSC queue、MPMC 队列——列为未来优化）
- 交错式 Rerank（实时更新 TopK 阈值——列为未来优化）
- O_DIRECT（初期 buffered I/O）
- Early termination 优化
- Warmup 的 mlock/madvise（初期只做 EnsureClusterLoaded）
- Benchmark 可执行文件（后续 PR）
- ConANN 停止阈值和 epsilon 的采样计算（Warmup 阶段，后续实现；初步测试硬编码阈值）
- ALL 阈值自适应（根据运行时 I/O 延迟动态调整）
- 多列 Datum 序列（初期单 Datum 值）

## Impact

### Files to Create
| 文件 | 内容 |
|------|------|
| `include/vdb/query/async_reader.h` | AsyncReader 接口（`PrepRead` + `Submit` + `Poll` + `WaitAndPoll`）+ IoCompletion + IoUringReader + PreadFallbackReader |
| `src/query/io_uring_reader.cpp` | io_uring 实现（PrepRead → io_uring_prep_read + set_data，Submit → io_uring_submit） |
| `src/query/pread_fallback_reader.cpp` | pread 同步 fallback（PrepRead 记录 pending，Submit 批量同步执行） |
| `include/vdb/query/result_collector.h` + `.cpp` | CollectorEntry (独立 struct) + Top-K max-heap (distance + AddressEntry, 无 VecID) |
| `include/vdb/query/search_context.h` | 查询上下文 + SearchConfig(含 cq_entries=4096, safein_all_threshold) + SearchStats |
| `include/vdb/query/search_results.h` | SearchResult{distance, Datum* payload} + SearchResults (RAII, move-only) |
| `include/vdb/query/rerank_consumer.h` + `.cpp` | ConsumeVec/ConsumeAll(拆分ALL buffer)/ConsumePayload + payload_cache(统一offset 0) + 惰性释放 |
| `include/vdb/query/overlap_scheduler.h` + `.cpp` | PendingRead + pending_ map + InferType + ProbeAndSubmit + DrainAndRerank |
| `include/vdb/query/buffer_pool.h` + `.cpp` | Buffer 分配/回收池 |
| `include/vdb/common/distance.h` + `src/common/distance.cpp` | L2Sqr 薄包装（委托 simd::L2Sqr） |
| `tests/query/` | 各组件单元测试 + 端到端测试（硬编码 ConANN 阈值） |
| `tests/common/distance_test.cpp` | L2Sqr 单元测试 |

### Files to Modify
| 文件 | 修改 |
|------|------|
| `include/vdb/common/types.h` | 精简 ReadTaskType 为 VEC_ONLY/ALL/PAYLOAD；删除 FRONT/BACK；删除 `ReadTask` 和 `CompletedRead` 结构体；删除 `Candidate::vec_id` |
| `tests/common/types_test.cpp` | 删除 ReadTask、CompletedRead 相关测试；更新 Candidate 测试 |
| `include/vdb/storage/cluster_store.h` | 新增 codes_buffer 全量缓存 + GetCodePtr |
| `src/storage/cluster_store.cpp` | 实现 codes 缓存（EnsureClusterLoaded 一次性读取） |
| `include/vdb/storage/data_file_reader.h` | 新增 `int fd() const` public 访问器 |
| `CMakeLists.txt` | 新增 `vdb_query` 库目标，链接 liburing (REQUIRED)，无 Taskflow |
