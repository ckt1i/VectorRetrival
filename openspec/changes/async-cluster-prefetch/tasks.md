# Tasks: Async Cluster Prefetch (Phase 8)

## Batch 1: ParsedCluster + ClusterStoreReader 最小改动 (顺序执行)

- [x] Task 1: 定义 ParsedCluster + ClusterBlockLocation
  - 新建 `include/vdb/query/parsed_cluster.h`:
    - `ClusterBlockLocation` 结构体: `offset (uint64_t)`, `size (uint64_t)`, `num_records (uint32_t)`
    - `ParsedCluster` 结构体: `block_buf (unique_ptr<uint8_t[]>)`, `codes_start (const uint8_t*)`, `code_entry_size (uint32_t)`, `num_records (uint32_t)`, `decoded_addresses (vector<AddressEntry>)`
    - ParsedCluster 为 move-only（禁止拷贝）

- [x] Task 2: ClusterStoreReader 新增 3 个接口
  - 修改 `include/vdb/storage/cluster_store.h`:
    - 新增 `clu_fd() const → int`
    - 新增 `GetBlockLocation(cluster_id) → optional<ClusterBlockLocation>`
    - 新增 `ParseClusterBlock(cluster_id, block_buf, block_size, &out) → Status`
    - 不修改 ClusterData、EnsureClusterLoaded、GetCodePtr 等现有接口
  - 修改 `src/storage/cluster_store.cpp`:
    - 实现 `clu_fd()`: 返回 `fd_`
    - 实现 `GetBlockLocation()`: 从 `cluster_index_` 查 lookup_table index → 取 `block_offset`, `block_size`, `num_records`
    - 实现 `ParseClusterBlock()`: 从 raw block buffer 解析 mini-trailer → 设置 `codes_start = block_buf.get()` + `code_entry_size` → 解析 address blocks → SIMD decode addresses → 填充 `ParsedCluster`。复用 EnsureClusterLoaded 中的解析逻辑但输出到 ParsedCluster
  - 修改 `include/vdb/storage/segment.h`:
    - 转发 `clu_fd()`, `GetBlockLocation()`, `ParseClusterBlock()`

- [x] Task 3: ClusterStoreReader 新接口单元测试
  - 修改 `tests/storage/cluster_store_test.cpp`:
    - 新增 `clu_fd` 测试: 打开后 fd >= 0
    - 新增 `GetBlockLocation` 测试: 验证返回的 offset/size/num_records 与 Writer 写入一致
    - 新增 `ParseClusterBlock` 测试: 手动 pread 整个 block → ParseClusterBlock → 验证 codes_start 非空、num_records 正确、decoded_addresses 数量正确且与 EnsureClusterLoaded + GetAddress 结果一致

## Batch 2: OverlapScheduler 改造 (顺序执行，依赖 Batch 1)

- [x] Task 4: SearchConfig 扩展 + PendingIO 定义 + 头文件改造
  - 修改 `include/vdb/query/search_context.h`:
    - SearchConfig 新增: `prefetch_depth = 16`, `refill_threshold = 2`, `refill_count = 2`
  - 修改 `include/vdb/query/overlap_scheduler.h`:
    - 替换 `PendingRead` 为 `PendingIO` (含 Type enum: CLUSTER_BLOCK, VEC_ONLY, VEC_ALL, PAYLOAD)
    - 新增成员: `ready_clusters_ (unordered_map<uint32_t, ParsedCluster>)`, `next_to_submit_`, `inflight_clusters_`
    - 新增方法声明: `PrefetchClusters`, `ProbeAndDrainInterleaved`, `FinalDrain`, `DispatchCompletion`, `SubmitClusterRead`, `ProbeCluster(const ParsedCluster&, cid, ctx, reranker)`
    - 删除旧的 `ProbeAndSubmit`, `DrainAndRerank`, `InferType` 声明
    - 保留 `FetchMissingPayloads`, `AssembleResults`

- [x] Task 5: 实现 SubmitClusterRead + PrefetchClusters
  - 修改 `src/query/overlap_scheduler.cpp`:
    - 实现 `SubmitClusterRead(cluster_id)`: GetBlockLocation → new[] → PrepRead(.clu fd) → pending_ insert {CLUSTER_BLOCK} → inflight_clusters_++
    - 实现 `PrefetchClusters`: 遍历 sorted_clusters[0..prefetch_depth), 调用 SubmitClusterRead → Submit
    - 注：cluster block buffer 使用 new[] 而非 BufferPool，避免所有权冲突

- [x] Task 6: 实现 DispatchCompletion + ProbeCluster
  - 修改 `src/query/overlap_scheduler.cpp`:
    - 实现 `DispatchCompletion`: 从 pending_ 取出 PendingIO → 按 type 分发:
      - CLUSTER_BLOCK: inflight_clusters_--, 构造 unique_ptr 接管 buf, ParseClusterBlock → ready_clusters_（不 Release buffer）
      - VEC_ONLY: ConsumeVec → Release
      - VEC_ALL: ConsumeAll → Release
      - PAYLOAD: ConsumePayload（所有权转移）
    - 实现 `ProbeCluster(const ParsedCluster& pc, cid, ctx, reranker)`:
      - 从 pc.codes_start + idx * pc.code_entry_size 取 code（替代 GetCodePtr）
      - 从 pc.decoded_addresses[idx] 取 address（替代 GetAddress）
      - 其余逻辑与原 ProbeAndSubmit per-cluster 相同
      - PrepRead 使用 PendingIO{VEC_ONLY/VEC_ALL} 标记类型

- [x] Task 7: 实现 ProbeAndDrainInterleaved + FinalDrain + 重写 Search
  - 修改 `src/query/overlap_scheduler.cpp`:
    - 实现 `ProbeAndDrainInterleaved`: 按距离顺序遍历 sorted_clusters → while cid not in ready_clusters_ 循环 WaitAndPoll+DispatchCompletion → ProbeCluster(ready_clusters_[cid]) → erase(cid) → Submit → 滑动窗口补充检查
    - 实现 `FinalDrain`: while pending_ not empty → WaitAndPoll → DispatchCompletion
    - 重写 `Search()`: 重置 per-query 状态 → PrefetchClusters → ProbeAndDrainInterleaved → FinalDrain → Finalize → FetchMissingPayloads → AssembleResults
    - 删除旧的 `ProbeAndSubmit`, `DrainAndRerank`, `InferType` 实现

- [x] Task 8: 更新 FetchMissingPayloads
  - 修改 `src/query/overlap_scheduler.cpp`:
    - FetchMissingPayloads: PendingRead → PendingIO{PAYLOAD}, drain 循环使用 DispatchCompletion
    - AssembleResults: 不变

## Batch 3: 测试 (依赖 Batch 1 + 2)

- [x] Task 9: 更新端到端测试
  - 修改 `tests/query/overlap_scheduler_test.cpp`:
    - 现有 EndToEnd_PreadFallback 和 StatsArePopulated 测试直接通过
    - 新增 PrefetchConfig_SmallDepth: prefetch_depth=4, nprobe=4
    - 新增 PrefetchDepth_ExceedsNprobe: prefetch_depth=100, nprobe=4
    - 新增 MultipleQueries_StateReset: 10 个连续查询验证状态重置

- [x] Task 10: 编译并运行所有测试
  - 31/31 tests passed, 0 failures
  - 包括修复已有 GetCodePtr_MatchesLoadCode 测试中的 API 不匹配问题
