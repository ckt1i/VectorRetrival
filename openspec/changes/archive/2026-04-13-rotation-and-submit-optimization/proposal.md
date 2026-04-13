## Why

perf profiling (nprobe=200, COCO 100K, 500 queries) 揭示了两个主要性能瓶颈：

1. **RotationMatrix 序列化丢失 FWHT 标志**：`rotation.bin` 不保存 `use_fast_hadamard_` 和 `diag_signs_`，导致加载后每�� `Apply()` 走 O(dim²) dense matmul 而非 O(dim log dim) FWHT。ProbeCluster 每个 cluster 调用一次��nprobe=200 时贡献 **0.74ms/query（占总延迟 29%）**。同时因��没有 `rotated_centroids.bin`，`used_hadamard_` 为 false，无法走 `PrepareQueryRotatedInto` 快速路径。

2. **io_uring Submit 过于频繁**：每个 ProbeCluster 后立即调用 `reader_.Submit()`（一次 `io_uring_enter` 系统调用），nprobe=200 时约 200 次 syscall，贡献 **0.461ms/query（占总延迟 18%）**。

两者合计 1.2ms，占 2.51ms 总延迟的 47%。修复后预期可将 nprobe=200 延迟从 2.51ms 降至 ~0.9ms。

## What Changes

- **RotationMatrix 序列化格式变更（不向后兼容）**：Save/Load 直接写入新格式，包含 `use_fast_hadamard_` 标志和 `diag_signs_`。旧 rotation.bin 不再兼容，需重建索引
- **重建 COCO 100K 索引**：使用当前 builder（已支持 `rotated_centroids.bin` 写入）重新构建索引，确保 `has_rotated_centroids=true` 和新格式 rotation.bin 同时生效
- **io_uring Submit 批量化**：ProbeAndDrainInterleaved 中将 per-cluster Submit 改为每 N 个 cluster 批量 Submit，减少系统调用次数
- **IoUringReader 暴露 prepped() 接口**：确保 WaitAndPoll 前能检查未提交 SQE，防止死锁
- **方案 A 效果验证**：在实施方案 B（Submit 批量化��之前，先在新索引上运行 nprobe sweep benchmark 验证方案 A 的实际收益和新的 timing 分布

## Capabilities

### New Capabilities
- `rotation-hadamard-restore`: RotationMatrix 新序列化格式（含 FWHT 标志 + diag_signs）+ 索引重建验证
- `submit-batching`: io_uring Submit 批量化策略，减少 per-query 系统调用次数

### Modified Capabilities

（无现有 spec 需要修改）

## Impact

- **文件格式**：`rotation.bin` 新格式不向后兼容，旧索引需要重建
- **索引构建**：使用现有 builder 重建 COCO 100K 索引，新索引包含 `rotated_centroids.bin` + 新格式 `rotation.bin`
- **搜索路径**：`OverlapScheduler::ProbeCluster` 从 `PrepareQueryInto`（200× dense matmul）切换到 `PrepareQueryRotatedInto`（200× subtract-only）
- **io_uring 调度**：`ProbeAndDrainInterleaved` 中 Submit 调用频率降低，需确保 WaitAndPoll 前 flush 未提�� SQE
- **涉及文件**：`rabitq_rotation.{h,cpp}`, `overlap_scheduler.cpp`, `async_reader.h`, `io_uring_reader.cpp`, `search_context.h`, `benchmarks/bench_e2e.cpp`
