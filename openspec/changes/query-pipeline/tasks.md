# Tasks: Query Pipeline (Phase 7)

## Prerequisites

- [ ] Task 1: 精简 `ReadTaskType` + 清理 types.h
  - 将 `ReadTaskType` 从 5 种 (ALL/FRONT/VEC_ONLY/BACK/PAYLOAD) 精简为 3 种 (VEC_ONLY/ALL/PAYLOAD)
  - 移除 FRONT、BACK（不再使用大小切分方案）
  - 清理 `Candidate` 结构中的 `VecID` 字段（如不再需要）
  - 更新 `ReadTaskTypeName` 函数

- [ ] Task 2: 扩展 `ClusterStoreReader` codes 缓存
  - 在 `ClusterData` 中新增 `codes_buffer` 字段
  - 修改 `EnsureClusterLoaded` 一次性读取整个 codes region 到内存
  - 新增 `GetCodePtr(cluster_id, record_idx)` 方法返回指针
  - 更新 `cluster_store_test.cpp` 验证 `GetCodePtr` 与 `LoadCode` 一致性

- [ ] Task 3: 实现 `L2Sqr` 精确距离计算
  - 创建 `include/vdb/common/distance.h` + `src/common/distance.cpp`
  - `float L2Sqr(const float* a, const float* b, uint32_t dim)`
  - 初期标量实现，后续可加 SIMD
  - 创建 `tests/common/distance_test.cpp`

## Core Components

- [ ] Task 4: 实现 `AsyncReader` 接口 + `PreadFallbackReader`
  - 创建 `include/vdb/query/async_reader.h`（定义 `IoRequest`, `IoCompletion`, `AsyncReader` 接口）
  - `IoRequest`: fd, offset, length, buffer; `user_data = (uint64_t)buffer`
  - `IoCompletion`: buffer（从 CQE user_data 还原）, result（字节数或负 errno）
  - 创建 `src/query/pread_fallback_reader.cpp`（同步 pread 实现：Submit 立即执行 pread，结果存入内部队列）
  - 创建 `tests/query/pread_fallback_reader_test.cpp`

- [ ] Task 5: 实现 `IoUringReader`
  - 创建 `src/query/io_uring_reader.cpp`
  - 实现 `Init(queue_depth=64)`, `Submit`, `Poll`, `WaitAndPoll`, `InFlight`
  - Submit 时 `io_uring_sqe_set_data(sqe, (void*)request.buffer)` 设置 user_data
  - Poll 时 `cqe->user_data` 还原为 `uint8_t*` buffer 指针
  - 运行时 io_uring 初始化失败时打印警告，由 Scheduler 降级到 PreadFallback
  - 创建 `tests/query/io_uring_reader_test.cpp`

- [ ] Task 6: 实现 `ReadQueue`
  - 创建 `include/vdb/query/read_queue.h` + `src/query/read_queue.cpp`
  - 双队列 Qv/Qp，`Push`(→Qv), `PushQp`(→Qp), `PopBatch`(Qv 优先), `QvFull`
  - Qv 容量上限 256（back-pressure）
  - 创建 `tests/query/read_queue_test.cpp`

- [ ] Task 7: 实现 `ResultCollector`
  - 创建 `include/vdb/query/result_collector.h` + `src/query/result_collector.cpp`
  - max-heap 存 `(float distance, AddressEntry addr)`，无 VecID
  - `TryInsert(float, AddressEntry)`, `TopDistance`, `Full`, `Size`, `Finalize`（返回 distance 升序）
  - 创建 `tests/query/result_collector_test.cpp`

- [ ] Task 8: 实现 `SearchContext` + `SearchResults`
  - 创建 `include/vdb/query/search_context.h`
  - 定义 `SearchConfig`（含 `safein_all_threshold=256KB`, `qv_capacity=256`, `io_queue_depth=64`, `cluster_atomic_threshold=1024`）
  - 定义 `SearchStats`（含 probe/io/rerank 计数和耗时）
  - 定义 `SearchContext` 类（持有 query_vec, config, stats, collector）
  - 创建 `include/vdb/query/search_results.h`
  - 定义 `SearchResult{float distance, Datum* payload}`（Datum 初期为单个值，非多列序列）
  - 定义 `SearchResults`（RAII 包装）：禁止拷贝，允许移动，析构释放 owned_buffers
  - 迭代器接口: begin/end/size/operator[]

- [ ] Task 9: 实现 `BufferPool`
  - 创建 `include/vdb/query/buffer_pool.h` + `src/query/buffer_pool.cpp`
  - `Acquire(size)` / `Release(buf)`
  - 简单实现：空池时 new，Release 归还到 pool
  - 创建 `tests/query/buffer_pool_test.cpp`

## Integration

- [ ] Task 10: 实现 `RerankConsumer`
  - 创建 `include/vdb/query/rerank_consumer.h` + `src/query/rerank_consumer.cpp`
  - `ConsumeVec(buffer, addr)`: 解析向量 → L2Sqr → TryInsert → 调用方释放 buffer
  - `ConsumeAll(buffer, addr)`: 解析向量 → L2Sqr → TryInsert → 进 TopK 则 memcpy payload 到新 buffer 入 cache
    - 调用方始终释放原始 ALL buffer（ConsumeAll 不持有原始 buffer 所有权）
  - `ConsumePayload(buffer, addr)`: buffer 转移到 payload_cache（调用方不释放）
  - `HasPayload`, `TakePayload`, `CachePayload`, `CleanupUnusedCache`（惰性释放）
  - 内部维护 `payload_cache_: unordered_map<uint64_t, unique_ptr<uint8_t[]>>`
  - 所有 cache 条目的 payload 均从 offset 0 开始（ALL 已拆分，PAYLOAD 天然如此）
  - 创建 `tests/query/rerank_consumer_test.cpp`

- [ ] Task 11: 实现 `OverlapScheduler`
  - 创建 `include/vdb/query/overlap_scheduler.h` + `src/query/overlap_scheduler.cpp`
  - 实现 `PendingRead` 结构 + `pending_: unordered_map<uint8_t*, PendingRead>`
  - Submit 时插入 pending_，Poll 完成时 extract 并 `InferType` 推断 ReadTaskType
  - 实现交错式主循环 `InterleavedLoop`: Probe → Submit(+pending_) → Poll → InferType → 3 路分支
  - Probe 内部: SafeIn ≤256KB→ALL(Qv), SafeIn >256KB→VEC_ONLY(Qv)+PAYLOAD(Qp), Uncertain→VEC_ONLY(Qv)
  - Phase 2: `FetchMissingPayloads` — Uncertain 进 TopK 条目的 payload 批量补读
  - Phase 3: `AssembleResults` — 从 payload_cache 组装 SearchResults (RAII)，所有 payload 从 offset 0 取
  - 内部持有 ReadQueue、BufferPool、RerankConsumer、pending_
  - 创建 `tests/query/overlap_scheduler_test.cpp`
    - 初步测试: 硬编码 ConANN 阈值 + PreadFallbackReader + 小规模索引端到端测试
    - 后续: 集成 warmup 采样计算 ConANN 阈值进行进一步测试

## Build & Test

- [ ] Task 12: 更新 `CMakeLists.txt`
  - 新增 `vdb_query` 库目标
  - 源文件: io_uring_reader, pread_fallback_reader, read_queue, result_collector, rerank_consumer, overlap_scheduler, buffer_pool, distance
  - 链接: `vdb_index`, `vdb_storage`, `vdb_simd`, `liburing`
  - 新增所有 query 测试可执行文件

- [ ] Task 13: 编译并运行所有测试
  - 确保全部 query 测试通过 + 原有测试无回归
  - 修复发现的问题
