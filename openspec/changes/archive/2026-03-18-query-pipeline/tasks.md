# Tasks: Query Pipeline (Phase 7)

## Batch 1: Prerequisites (互相独立，可并行)

- [x] Task 1: 精简 `ReadTaskType` + 清理 types.h
  - `ReadTaskType` 从 5 种精简为 3 种：保留 `ALL`, `VEC_ONLY`, `PAYLOAD`；删除 `FRONT`, `BACK`
  - 更新 `ReadTaskTypeName()` 函数，删除 FRONT/BACK case
  - 删除 `ReadTask` 结构体（新模型中 Probe 直接调用 PrepRead，不再通过队列传递 ReadTask）
  - 删除 `CompletedRead` 结构体（被 `IoCompletion` + `pending_` map 取代）
  - 删除 `Candidate::vec_id` 字段（查询流水线不使用 VecID）
  - 更新 `tests/common/types_test.cpp`：删除 ReadTask、CompletedRead 相关测试；更新 Candidate 测试

- [x] Task 2: 扩展 `ClusterStoreReader` codes 缓存
  - 在 `ClusterData` 中新增 `codes_buffer` 字段
  - 修改 `EnsureClusterLoaded` 一次性读取整个 codes region 到内存
  - 新增 `GetCodePtr(cluster_id, record_idx)` 方法返回指针
  - 在 `Segment` 中转发 `GetCodePtr`
  - 更新 `cluster_store_test.cpp` 验证 `GetCodePtr` 与 `LoadCode` 数据一致性

- [x] Task 3: 实现 `L2Sqr` 精确距离计算
  - 创建 `include/vdb/common/distance.h` + `src/common/distance.cpp`
  - `float L2Sqr(const float* a, const float* b, uint32_t dim)`
  - 薄包装，委托 `simd::L2Sqr`（已有 AVX2 + 标量回退），不重复实现
  - 创建 `tests/common/distance_test.cpp`

## Batch 2: Core Components (互相独立，可并行)

- [x] Task 4: 实现 `AsyncReader` 接口 + `PreadFallbackReader`
  - 创建 `include/vdb/query/async_reader.h`
    - 定义 `IoCompletion{buffer, result}`
    - 定义 `AsyncReader` 接口：`PrepRead(fd, buf, len, offset)` + `Submit()` + `Poll()` + `WaitAndPoll()` + `InFlight()`
    - PrepRead 对应 `io_uring_prep_read` + `io_uring_sqe_set_data`
    - Submit 对应 `io_uring_submit`
  - 创建 `src/query/pread_fallback_reader.cpp`
    - PrepRead 记录到内部 pending list
    - Submit 批量同步执行所有 pending 的 pread，结果存入内部完成队列
    - InFlight 始终返回 0
  - 修改 `include/vdb/storage/data_file_reader.h`：新增 `int fd() const { return fd_; }` public 访问器
  - 创建 `tests/query/pread_fallback_reader_test.cpp`

- [x] Task 5: 实现 `IoUringReader`
  - 创建 `src/query/io_uring_reader.cpp`（class decl 在 async_reader.h 中）
  - `Init(queue_depth=64, cq_entries=4096)` → `io_uring_queue_init_params` + `IORING_SETUP_CQSIZE`
  - `PrepRead` → `io_uring_get_sqe` + `io_uring_prep_read` + `io_uring_sqe_set_data(sqe, buf)`
  - `Submit` → `io_uring_submit`，返回提交数量
  - `Poll` → `io_uring_peek_cqe` 循环
  - `WaitAndPoll` → `io_uring_wait_cqe` + 消费所有就绪 CQE
  - 维护 `in_flight_` 计数器
  - 运行时 io_uring 初始化失败时打印警告，由 Scheduler 降级到 PreadFallback
  - 创建 `tests/query/io_uring_reader_test.cpp`（io_uring 不可用时 GTEST_SKIP）

- [x] Task 6: 实现 `ResultCollector`
  - 创建 `include/vdb/query/result_collector.h` + `src/query/result_collector.cpp`
  - `CollectorEntry` 定义为独立 struct（非嵌套），RerankConsumer 和 OverlapScheduler 均引用
  - max-heap 存 `CollectorEntry{float distance, AddressEntry addr}`，无 VecID
  - `TryInsert(float, AddressEntry)`, `TopDistance`, `Full`, `Size`, `Finalize`（返回 distance 升序）
  - 创建 `tests/query/result_collector_test.cpp`

- [x] Task 7: 实现 `SearchContext` + `SearchResults`
  - 创建 `include/vdb/query/search_context.h`
  - 定义 `SearchConfig`：`top_k`, `nprobe`, `probe_batch_size=64`, `cluster_atomic_threshold=1024`, `io_queue_depth=64`, `cq_entries=4096`, `safein_all_threshold=256*1024`
  - 定义 `SearchStats`：probe/rerank 计数和耗时（无单独 io_time，因为 I/O 与 Probe/Rerank 重叠）
  - 定义 `SearchContext` 类（持有 query_vec, config, stats, collector）
  - 创建 `include/vdb/query/search_results.h`
  - 定义 `SearchResult{float distance, Datum* payload}`（Datum 初期单值）
  - 定义 `SearchResults`（RAII）：禁止拷贝，允许移动，析构释放 owned_buffers
  - 使用 `vdb::query` 命名空间避免与 `vdb::SearchResult`(types.h) 冲突

- [x] Task 8: 实现 `BufferPool`
  - 创建 `include/vdb/query/buffer_pool.h` + `src/query/buffer_pool.cpp`
  - `Acquire(size)`: 查找 pool 中 capacity >= size 的 buffer，无则 new
  - `Release(buf, size)`: 归还到 pool
  - 存储 `vector<pair<unique_ptr<uint8_t[]>, uint32_t>>` (buffer + capacity)
  - 创建 `tests/query/buffer_pool_test.cpp`

## Batch 3: Integration (顺序依赖)

- [x] Task 9: 实现 `RerankConsumer`
  - 依赖: Task 3 (L2Sqr), Task 6 (ResultCollector), Task 7 (SearchContext)
  - 创建 `include/vdb/query/rerank_consumer.h` + `src/query/rerank_consumer.cpp`
  - `ConsumeVec(buffer, addr)`: 解析向量 → L2Sqr → TryInsert → 调用方释放 buffer
  - `ConsumeAll(buffer, addr)`: 解析向量 → L2Sqr → TryInsert → 进 TopK 则 memcpy payload 到新 buffer 入 cache；调用方始终释放原始 ALL buffer
  - `ConsumePayload(buffer, addr)`: buffer 转移到 payload_cache（调用方不释放）
  - `HasPayload`, `TakePayload`, `CachePayload`, `CleanupUnusedCache`（惰性释放）
  - 内部 `payload_cache_: unordered_map<uint64_t, unique_ptr<uint8_t[]>>`
  - 所有 cache 条目的 payload 均从 offset 0 开始
  - 创建 `tests/query/rerank_consumer_test.cpp`

- [x] Task 10: 实现 `OverlapScheduler`
  - 依赖: 所有前置 Tasks (1–9)
  - 创建 `include/vdb/query/overlap_scheduler.h` + `src/query/overlap_scheduler.cpp`
  - 实现 `PendingRead{addr, read_offset, read_length}` + `pending_: unordered_map<uint8_t*, PendingRead>`
  - `Search(query_vec)` → ProbeAndSubmit → DrainAndRerank → FetchMissingPayloads → AssembleResults
  - `ProbeAndSubmit`: 遍历所有 cluster，mini-batch 64 EstimateBatch → Classify → PrepRead → flush Submit per cluster
  - `DrainAndRerank`: while pending_ not empty → WaitAndPoll → extract → InferType → ConsumeVec/All/Payload
  - `FetchMissingPayloads`: Uncertain 进 TopK 但无 cached payload → 批量 PrepRead + Submit → drain
  - `AssembleResults`: TakePayload → ParseDatum → RAII SearchResults
  - `InferType`: read_length==dim*4 → VEC_ONLY, offset==addr.offset && length==addr.size → ALL, else → PAYLOAD
  - 成员: index_, reader_, config_, buffer_pool_, reranker_, pending_（全部单线程访问，无锁）
  - 创建 `tests/query/overlap_scheduler_test.cpp`
    - 用 IvfBuilder 构建小索引（256 条 64 维，4 聚类）
    - PreadFallbackReader + Search → 与 brute-force L2 TopK 对比
    - 硬编码 ConANN 阈值

## Batch 4: Build & Test

- [x] Task 11: 更新 `CMakeLists.txt`
  - 新增 `vdb_query` 库目标
  - 源文件: io_uring_reader, pread_fallback_reader, result_collector, rerank_consumer, overlap_scheduler, buffer_pool, distance
  - 链接: `vdb_index`, `vdb_storage`, `vdb_simd`, `liburing`（无 Taskflow）
  - 新增 7 个测试可执行文件: pread_fallback_reader_test, io_uring_reader_test, result_collector_test, buffer_pool_test, rerank_consumer_test, overlap_scheduler_test, distance_test

- [x] Task 12: 编译并运行所有测试
  - 确保全部 query 测试通过 + 原有测试无回归
  - 修复发现的问题
