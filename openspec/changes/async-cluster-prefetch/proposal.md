# Proposal: Async Cluster Prefetch (Phase 8)

## What

将聚类数据加载从同步 pread 改为 io_uring 异步预取，采用滑动窗口机制与现有查询流水线集成。核心改动：

1. **独立 ParsedCluster 结构** — 新建 `ParsedCluster` struct，独立于 `ClusterStoreReader::ClusterData`。codes 区域零拷贝指向 block buffer 内部，addresses SIMD 解码后独立存储。查询路径完全绕过 `loaded_clusters_`
2. **ClusterStoreReader 最小扩展** — 仅新增 3 个接口：`clu_fd()`、`GetBlockLocation()`、`ParseClusterBlock()`。不修改任何现有接口或内部结构
3. **OverlapScheduler 滑动窗口预取** — 查询开始时批量提交 `prefetch_depth`（默认 16）个 cluster block 读取打满 I/O 带宽；按距离顺序依次 probe 已就绪 cluster；每 probe 完一个 cluster 后检查剩余 inflight，低于 `refill_threshold`（默认 2）时补充提交
4. **统一事件循环** — cluster block CQE 与原始向量 CQE 共享同一个 io_uring ring 和 WaitAndPoll 循环，通过 PendingIO 类型标记区分处理路径
5. **严格距离顺序 probe** — 通过 `ready_clusters_` 缓冲层，确保始终按 FindNearestClusters 返回的距离顺序 probe，最大化 ConANN SafeOut 剪枝效果。ProbeCluster 直接操作 ParsedCluster，probe 完即释放

## Why

Phase 7 的查询流水线中，`EnsureClusterLoaded` 使用同步 pread 逐个加载聚类数据，是查询路径上唯一的串行阻塞点。对于 nprobe=32 的场景，首次查询需要串行执行 32 次同步 pread 加载 cluster block（每次包含多次 pread：trailer + codes + address blocks），完全无法利用 NVMe 的并行 I/O 能力。

将 cluster 加载纳入 io_uring 后：
- **冷启动延迟降低**：nprobe 个 cluster 并行读取，理论 I/O 延迟从 O(nprobe) 降至 O(1)（受限于 I/O 带宽而非延迟）
- **CPU/IO 全程重叠**：在等待后续 cluster 读取完成的同时，已就绪 cluster 的 probe 和前序 cluster 的 rerank 同步进行
- **整块读取减少系统调用**：每个 cluster 从当前 3~N 次 pread 合并为 1 次 io_uring read（利用 lookup_table 中已有的 block_offset + block_size）

## Scope

### In Scope
- 新建 `ParsedCluster` 结构（独立于 ClusterStoreReader::ClusterData）
- ClusterStoreReader 新增 `clu_fd()`、`GetBlockLocation()`、`ParseClusterBlock()` 接口
- Segment 转发新增接口
- OverlapScheduler 新增滑动窗口预取逻辑 + 统一事件循环
- ProbeCluster 直接操作 ParsedCluster（绕过 GetCodePtr/GetAddress/loaded_clusters_）
- PendingIO 类型扩展（CLUSTER_BLOCK / VEC_ONLY / VEC_ALL / PAYLOAD）
- SearchConfig 新增 prefetch_depth、refill_threshold、refill_count 参数
- 单元测试 + 端到端测试

### Out of Scope
- 修改 ClusterStoreReader::ClusterData 或 EnsureClusterLoaded（保持原样）
- loaded_clusters_ 参与查询路径（本次单查询场景不使用）
- 多查询间的 cluster 缓存共享
- 预加载所有 cluster（Open 时全量加载）
- O_DIRECT 支持
- 乱序 probe 优化（本次严格按距离顺序）
- 多线程并发

## Impact

### Files to Create
| 文件 | 说明 |
|------|------|
| `include/vdb/query/parsed_cluster.h` | ParsedCluster + ClusterBlockLocation 定义 |

### Files to Modify
| 文件 | 修改 |
|------|------|
| `include/vdb/storage/cluster_store.h` | 新增 clu_fd、GetBlockLocation、ParseClusterBlock 声明（不修改现有接口） |
| `src/storage/cluster_store.cpp` | 实现 3 个新接口 |
| `include/vdb/storage/segment.h` | 转发 clu_fd、GetBlockLocation、ParseClusterBlock |
| `include/vdb/query/overlap_scheduler.h` | PendingIO 替代 PendingRead；新增 ready_clusters_ (ParsedCluster)；新增方法声明 |
| `src/query/overlap_scheduler.cpp` | 重写 Search 流程：PrefetchClusters → 统一事件循环 → FinalDrain → FetchPayloads → Assemble |
| `include/vdb/query/search_context.h` | SearchConfig 新增 prefetch_depth、refill_threshold、refill_count |
| `tests/storage/cluster_store_test.cpp` | 新增 ParseClusterBlock 单元测试 |
| `tests/query/overlap_scheduler_test.cpp` | 更新端到端测试验证异步加载路径 |
