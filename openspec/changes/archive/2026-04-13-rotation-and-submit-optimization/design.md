## Context

perf stat + perf record 在 COCO 100K (nprobe=200, 500 queries) 上的结果：

| 指标 | 值 |
|------|-----|
| IPC | 1.54 |
| L1-dcache-misses | 2784M |
| cache-miss rate | 6.56% |
| RunQueryRound 占总 cycles | 21.4% |
| RotationMatrix::Apply 占 RunQueryRound | 56% (dense O(dim²) matmul) |
| io_uring_enter 占总 cycles | 4.8% |

当前 per-query timing (nprobe=200, WARM):
- total=2.51ms, probe=1.75ms, uring_submit=0.46ms, parse_cluster=0.05ms, uring_prep=0.02ms, fetch_missing=0.01ms, io_wait=0.002ms, rerank=0.01ms

根因：
1. `RotationMatrix::Save()` 不保存 `use_fast_hadamard_` / `diag_signs_` → Load 后 Apply 走 dense O(n²) path
2. 旧索引没有 `rotated_centroids.bin` → `used_hadamard_=false` → 每个 cluster 调用 `PrepareQueryInto`（含 Apply）而非 `PrepareQueryRotatedInto`（仅 subtract）
3. `ProbeAndDrainInterleaved` 每个 cluster 后立刻 `reader_.Submit()` → ~200 次 `io_uring_enter` syscall

## Goals / Non-Goals

**Goals:**
- 将 nprobe=200 per-query 延迟从 2.51ms 降至 ~0.9ms
- 新格式 `rotation.bin` 直接保存完整 Hadamard 状态（不向后兼容）
- 重建 COCO 100K 索引（新 rotation.bin + rotated_centroids.bin），在新索引上运行所有测试
- Submit 批量化在方案 A 验证后再实施，避免盲目优化

**Non-Goals:**
- 不保持旧 rotation.bin 向后兼容（旧索引需重建）
- 不修改 FastScan / ExRaBitQ 内核（它们在 perf 中占比很小）
- 不修改 CRC 校准逻辑（一次性开销，不影响 per-query 延迟）
- 不引入 io_uring SQPOLL 模式（需要 root 权限）

## Decisions

### D1: RotationMatrix 序列化格式（不向后兼容）

**选择**：新格式直接在 dense matrix 之后写入 Hadamard 状态，不做旧格式检测。

**格式**：
```
[dim:u32][data: dim×dim floats][flags: u8][diag_signs: dim × int8]
```
- `flags` bit 0 = `use_fast_hadamard_`（1=Hadamard, 0=General）
- 当 `flags & 1 == 0` 时，无 `diag_signs` 段（General rotation 不需要）
- 当 `flags & 1 == 1` 时，后跟 dim 个 int8 diag_signs

**Load 逻辑**：读取 dim + data 后，继续读 1 字节 flags。若 `flags & 1`，再读 dim 个 int8 diag_signs，设置 `use_fast_hadamard_=true`。

**替代方案**：
- 向后兼容 magic trailer → **否决**：用户决定不需要向后兼容，简化实现
- 只存 diag_signs 不存 dense matrix → **否决**：ApplyInverse 和调试仍需 dense matrix

### D2: 重建索引而非自动检测

**选择**：使用现有 `IvfBuilder::WriteIndex` 重建 COCO 100K 索引。当前 builder 已经：
1. 对 power-of-2 dim 调用 `GenerateHadamard(seed, true)` 设置 `use_fast_hadamard_=true`
2. 写入 `rotated_centroids.bin`
3. 在 segment.meta 中设置 `has_rotated_centroids=true`

重建后 `IvfIndex::Open` 自动：
1. Load 新格式 rotation.bin → `use_fast_hadamard_=true` 恢复
2. 检测 `has_rotated_centroids=true` → 加载 `rotated_centroids.bin` → `used_hadamard_=true`
3. OverlapScheduler 自动走 `PrepareQueryRotatedInto` 快速路径

**替代方案**：
- Open 时自动检测 Hadamard + 内存生成 rotated_centroids → **否决**：用户选择重建索引，无需复杂检测逻辑

### D3: 方案 A 效果验证（在实施方案 B 之前）

**选择**：修复 Rotation 并重建索引后，先跑 `--nprobe-sweep 50,100,150,200 --queries 500` 对比修复前的 CSV 数据，确认：
1. recall 不变（rotation 只改计算路径，不改结果）
2. probe_time_ms 显著下降（预期从 1.75ms 降至 < 0.5ms）
3. 重新分析 timing 分布，确认 uring_submit 是否仍为主要瓶颈

**原因**：方案 A 修复后 ProbeCluster 可能降至 ~0.01ms/cluster，若 batch=8 的 IO 启动延迟 (8×0.01=0.08ms) 大于 syscall 节省 (7×0.002=0.014ms)，则方案 B 反而有害。需要实测数据决定 batch_size 或是否执行方案 B。

### D4: io_uring Submit 批量化

**选择**：ProbeAndDrainInterleaved 中引入 `submit_batch_size` 参数（默认 8），每 N 个 cluster probe 后才 Submit。在 WaitAndPoll 前检查 `reader_.prepped() > 0` 做 safety flush。

**关键约束**：
- cluster block prefetch（PrefetchClusters）的 Submit 保持不变（它在循环外，一次性提交）
- per-probe 产生的 vec_reads（SafeIn/Uncertain）是延迟不敏感的（只在 rerank 时消费），适合批量提交
- refill cluster reads 的 Submit 合并到 batch flush 中

**IoUringReader 接口变更**：
- 新增 `uint32_t prepped() const { return prepped_; }` — 暴露未提交 SQE 计数

## Risks / Trade-offs

| 风险 | 缓解措施 |
|------|----------|
| 旧索引不再兼容需重建 | 用户已确认可接受。COCO 100K 重建耗时 < 2 分钟 |
| 重建后 seed 不同可能导致 recall 变化 | 使用相同 seed=42 和参数重建，recall 应一致 |
| Submit 批量化可能推迟 cluster block IO 启动 | cluster blocks 由 PrefetchClusters 提前提交（不受 batching 影响）。只有 vec_reads 被延迟 |
| 方案 B 在方案 A 修复后可能收益不足 | 这正是为什么先验证方案 A 效果再决定方案 B 的参数或是否执行 |
