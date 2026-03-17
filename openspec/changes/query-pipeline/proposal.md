# Proposal: Query Pipeline (Phase 7)

## What

实现 VDB VectorRetrival 的查询流水线——从接收查询请求到返回 Top-K 结果的完整路径。采用交错式流水线架构，Probe / I/O / Rerank 深度重叠。核心组件包括：

1. **AsyncReader** — io_uring 异步 I/O 封装 + pread 同步 fallback（运行时降级）。CQE `user_data` 存储 buffer 指针，配合 `pending_` map 还原元信息
2. **ReadQueue** — 双优先级读取队列（Qv 高优先: ALL+VEC_ONLY / Qp 低优先: PAYLOAD），Qv 上限 256 back-pressure
3. **ResultCollector** — Top-K max-heap 收集器（存 distance + AddressEntry，无 VecID）
4. **SearchContext** — 查询上下文，持有 query、config（含 safein_all_threshold=256KB）、统计
5. **RerankConsumer** — 流式 Rerank 消费者 + payload_cache 管理。ALL buffer 在 ConsumeAll 中拆分（memcpy payload 到新 buffer），cache 统一 payload offset 0。被淘汰条目惰性释放
6. **OverlapScheduler** — 交错式 Probe + I/O + Rerank 调度主循环。内部维护 `pending_: unordered_map<uint8_t*, PendingRead>`，通过 `InferType` 从读取参数推断 ReadTaskType（无需显式存储 type）
7. **SearchResults** — RAII 结果包装，自动管理 payload buffer 生命周期。`SearchResult{distance, Datum* payload}`，Datum 初期为单个值（非多列序列）
8. **L2Sqr** — 精确 L2 距离计算（`common/distance.h`），初期标量，后续 SIMD
9. **BufferPool** — I/O buffer 分配/回收池

## Why

Phase 0–6 已完成全部离线构建和存储层。目前系统可以构建索引但无法执行查询。Phase 7 是将系统从"只写"变为"可查询"的关键一步。

关键设计目标：
- **I/O 与计算深度重叠**：Probe（RaBitQ 距离估算）和 I/O（读取记录）在主循环中每轮交替执行
- **SafeIn 全量预读**：高置信度记录（≤256KB）一次 ALL 读取 vec+payload，省一半 IOPS；>256KB 拆分为 VEC_ONLY(Qv)+PAYLOAD(Qp)
- **流式 Rerank**：I/O 完成一条就精排一条，不等待所有完成。ALL buffer 即时拆分（memcpy payload），cache 统一格式
- **异步 I/O**：io_uring 提交+轮询，CQE user_data 存 buffer 指针 + pending_ map 还原元信息，单线程打满 SSD IOPS
- **RAII 结果管理**：SearchResults 自动管理 payload buffer 生命周期，Datum 初期为单值
- **初期单线程**：协作式调度，预留多线程扩展点
- **类型推断**：从 PendingRead 的 read_offset/read_length 推断 ReadTaskType，无需在 CQE 中显式传递

## Scope

### In Scope
- io_uring 异步读取（liburing 构建时必须，运行时 fallback 到 pread）
- CQE user_data = buffer 指针 + `pending_` map 元信息还原 + `InferType` 类型推断
- 双优先级读取队列（Qv: ALL+VEC_ONLY 上限 256, Qp: PAYLOAD 无上限）
- 3 种 ReadTaskType: VEC_ONLY / ALL / PAYLOAD（从 5 种精简）
- SafeIn ALL 全量读取（≤256KB 阈值，>256KB 拆分 VEC_ONLY+PAYLOAD）
- ALL buffer 拆分策略：ConsumeAll 中 memcpy payload 到新 buffer 入 cache，原始 buffer 始终释放
- Top-K 结果收集（无 VecID，只返回 distance + Datum payload）
- RAII SearchResults 包装（move-only，析构自动释放 payload buffer）
- 交错式 Probe + I/O + Rerank 调度循环（聚类 ≤1024 原子 probe，>1024 分 64 条 mini-batch）
- 流式 Rerank（精确 L2 距离验证）+ payload_cache（统一 payload offset 0，被淘汰条目惰性释放）
- Phase 2 Uncertain payload 批量补读
- ClusterStoreReader codes 全量缓存（解决 Probe 阶段逐条 pread 瓶颈）
- L2Sqr 精确距离计算（common/distance.h，初期标量）
- BufferPool（简单 new/release 池）
- Datum 初期为单个值（非多列序列）
- 单元测试 + 端到端测试（ConANN 阈值初步硬编码）

### Out of Scope
- 多线程（MPMC 队列、线程池）
- O_DIRECT（初期 buffered I/O）
- Early termination 优化
- Warmup 的 mlock/madvise（初期只做 EnsureClusterLoaded）
- Benchmark 可执行文件（后续 PR）
- ConANN 停止阈值和 epsilon 的采样计算（Warmup 阶段，后续实现；初步测试硬编码阈值）
- L2Sqr SIMD 优化（初期标量实现）
- ALL 阈值自适应（根据运行时 I/O 延迟动态调整）
- 多列 Datum 序列（初期单 Datum 值）

## Impact

### Files to Create
| 文件 | 内容 |
|------|------|
| `include/vdb/query/async_reader.h` | AsyncReader 接口 + IoRequest(user_data=buffer指针) + IoCompletion + IoUringReader + PreadFallbackReader |
| `src/query/io_uring_reader.cpp` | io_uring 实现（Submit 设 user_data=(uint64_t)buffer，Poll 从 CQE 还原） |
| `src/query/pread_fallback_reader.cpp` | pread 同步 fallback |
| `include/vdb/query/read_queue.h` + `.cpp` | 双优先级队列（Qv 上限 256, Qp 无上限） |
| `include/vdb/query/result_collector.h` + `.cpp` | Top-K max-heap (distance + AddressEntry, 无 VecID) |
| `include/vdb/query/search_context.h` | 查询上下文 + SearchConfig(含 safein_all_threshold) + SearchStats |
| `include/vdb/query/search_results.h` | SearchResult{distance, Datum* payload} + SearchResults (RAII, move-only) |
| `include/vdb/query/rerank_consumer.h` + `.cpp` | ConsumeVec/ConsumeAll(拆分ALL buffer)/ConsumePayload + payload_cache(统一offset 0) + 惰性释放 |
| `include/vdb/query/overlap_scheduler.h` + `.cpp` | PendingRead + pending_ map + InferType + 交错式调度主循环 |
| `include/vdb/query/buffer_pool.h` + `.cpp` | Buffer 分配/回收池 |
| `include/vdb/common/distance.h` + `src/common/distance.cpp` | L2Sqr 精确距离（标量） |
| `tests/query/` | 各组件单元测试 + 端到端测试（硬编码 ConANN 阈值） |
| `tests/common/distance_test.cpp` | L2Sqr 单元测试 |

### Files to Modify
| 文件 | 修改 |
|------|------|
| `include/vdb/common/types.h` | 精简 ReadTaskType 为 VEC_ONLY/ALL/PAYLOAD 三种；移除 FRONT/BACK；清理 Candidate 的 VecID |
| `include/vdb/storage/cluster_store.h` | 新增 codes_buffer 全量缓存 + GetCodePtr |
| `src/storage/cluster_store.cpp` | 实现 codes 缓存（EnsureClusterLoaded 一次性读取） |
| `CMakeLists.txt` | 新增 `vdb_query` 库目标，链接 liburing (REQUIRED) |
