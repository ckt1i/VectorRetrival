## Context

Phase 1 (`conann-crc-early-stop`) 实现了 CRC 框架的核心组件（CrcStopper、CrcCalibrator、bench_conann_crc），使用精确 L2 距离验证了 CRC 在 coco 数据集上的剪枝效果。本变更将 CRC 集成到 OverlapScheduler，同时引入动态 SafeOut 和 RaBitQ 距离标定。

当前 OverlapScheduler 早停逻辑 (`overlap_scheduler.cpp:258-265`):
```cpp
if (config_.early_stop &&
    ctx.collector().Full() &&
    ctx.collector().TopDistance() < index_.conann().d_k()) { break; }
```

## Goals / Non-Goals

**Goals:**
- CRC 早停替换 d_k 早停，与 RaBitQ estimate 距离空间一致
- 动态 SafeOut 分界线：随搜索进展逐步收紧，提高 SafeOut 比例
- CRC 标定使用 RaBitQ 估计距离，保证标定/推理距离空间一致
- Prefetch 参数化（initial=4, refill=2, max=16）
- Benchmark 验证：SafeOut 误杀率、端到端搜索时间、流水线重叠比例

**Non-Goals:**
- 修改 SafeIn 的静态 d_k 阈值
- batch 内部更新 dynamic_d_k（仅 cluster 间更新）
- GPU 加速
- 修改 CrcStopper 接口（保持 O(1) ShouldStop 不变）

---

## 架构概览：两个 Heap + 两层剪枝

```
┌──────────────────────────────────────────────────────────────────┐
│                    OverlapScheduler 改造后                        │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│   Heap A: est_heap_ (RaBitQ estimate, 新增)                       │
│   ├─ ProbeCluster 时用 RaBitQ dist 更新                           │
│   ├─ 仅在 cluster 之间更新 dynamic_d_k                             │
│   ├─ CrcStopper 读 est_heap_.top → 早停判断                        │
│   └─ ClassifyAdaptive 读 est_heap_.top → SafeOut 动态阈值          │
│                                                                  │
│   Heap B: collector_ (exact L2, 现有)                             │
│   ├─ RerankConsumer 完成 rerank 后更新                             │
│   └─ 最终 top-k 结果来源                                           │
│                                                                  │
│   Layer 1: Cluster-level early stop                              │
│   ├─ CrcStopper.ShouldStop(probed, est_heap_.top)                │
│   └─ 替换原 d_k 判断                                               │
│                                                                  │
│   Layer 2: Vector-level pruning                                  │
│   ├─ SafeOut: approx_dist > dynamic_d_k + 2·margin   (动态)       │
│   ├─ SafeIn:  approx_dist < static_d_k - 2·margin    (静态)       │
│   └─ Uncertain: else → fetch for rerank                          │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

---

## 详细设计

### 1. RaBitQ Estimate Heap (est_heap_)

OverlapScheduler 新增成员：
```cpp
// RaBitQ estimate top-k heap (max-heap by distance)
std::vector<std::pair<float, uint32_t>> est_heap_;
uint32_t est_top_k_;  // = config_.top_k
```

在 `ProbeCluster()` 中，对每个 vector 的 RaBitQ 估计距离同时更新 est_heap_：
```
for each vector in cluster:
    est_dist = estimator.EstimateDistanceRaw(...)
    // 现有: Classify → schedule I/O
    // 新增: 更新 est_heap_
    if est_heap_.size() < top_k:
        push(est_dist, vector_id)
    elif est_dist < est_heap_.front():
        pop + push
```

**关键决策**: dynamic_d_k 仅在 cluster 之间更新。ProbeCluster 开始时读取当前 est_heap_.top 作为本 cluster 的 dynamic_d_k，cluster 内部 batch 间不重算。理由：
- 简化实现，避免 cluster 内部状态变化导致的 SafeOut 不一致
- 一个 cluster 内部的距离分布通常比较集中，差异不大

### 2. ConANN.ClassifyAdaptive()

新增方法（conann.h + conann.cpp）：
```cpp
ResultClass ClassifyAdaptive(float approx_dist, float margin,
                              float dynamic_d_k) const;
// SafeOut: approx_dist > dynamic_d_k + 2 * margin   ← 动态
// SafeIn:  approx_dist < d_k_ - 2 * margin           ← 静态（使用构造时的 d_k_）
// Uncertain: else
```

**SafeIn 保持静态的理由**:
- SafeIn 意味着"一定要fetch原数据"，是保守操作
- 动态化 SafeIn 可能导致 heap 未满时错误地标记为 SafeIn
- 静态 d_k 的 SafeIn 已经足够保守

**SafeOut 动态化的好处**:
```
cluster 0 (est_heap 空): dynamic_d_k = static_d_k (fallback)
  → SafeOut 比例 ≈ 原始水平

cluster 3+ (est_heap 趋于稳定): dynamic_d_k ≈ real kth dist
  → SafeOut 比例显著提高，I/O 减少
```

### 3. CRC 标定使用 RaBitQ 距离

改造 CrcCalibrator：
- `ComputeScoresForQuery` 中将 `simd::L2Sqr(query, vec, dim)` 替换为 RaBitQ 估计距离
- 需要额外输入：RaBitQ codes（每个 cluster 的 1-bit 量化码）和 rotation matrix
- CrcCalibrator 接口新增参数：

```cpp
struct ClusterData {
    const float* vectors;     // 原始向量（用于 ground truth 计算时保留）
    const uint32_t* ids;
    uint32_t count;
    // 新增:
    const uint8_t* codes_block;   // RaBitQ encoded block (codes + norms)
    uint32_t code_entry_size;      // 每个 entry 的字节数
};

static std::pair<CalibrationResults, EvalResults> Calibrate(
    const Config& config,
    const float* queries, uint32_t num_queries, Dim dim,
    const float* centroids, uint32_t nlist,
    const std::vector<ClusterData>& clusters,
    const std::vector<std::vector<uint32_t>>& ground_truth,
    // 新增:
    const RotationMatrix& rotation);
```

`ComputeScoresForQuery` 内部改为:
```
for each cluster cid in sorted order:
    pq = estimator.PrepareQuery(query, centroids[cid], rotation)
    for each entry in cluster[cid]:
        read code_words + norm_oc from codes_block
        est_dist = estimator.EstimateDistanceRaw(pq, code_words, num_words, norm_oc)
        heap_update(est_dist, id)
    d_p = heap.top()  // RaBitQ 估计的 kth distance
```

**一致性保证**: 标定时用 RaBitQ 距离 → d_min/d_max 是 RaBitQ 距离空间的统计量 → 线上 CrcStopper 输入也是 RaBitQ 距离 → 空间一致。

### 4. OverlapScheduler 集成

#### 4a. ProbeCluster 改造

```
ProbeCluster(pc, cid, ctx, reranker):
    // 在 cluster 开始时读取 dynamic_d_k
    float dynamic_d_k = (est_heap_.size() >= est_top_k_)
        ? est_heap_.front().first        // RaBitQ estimate heap top
        : index_.conann().d_k();          // fallback 到静态 d_k

    pq = estimator.PrepareQuery(query, centroid, rotation)
    margin = 2 * r_max * pq.norm_qc * eps_ip

    for offset = 0; offset < num_records; offset += 128:  // batch_size=128
        // RaBitQ estimate batch
        for each entry in batch:
            dists[i] = EstimateDistanceRaw(...)

        // Classify + schedule I/O
        for each entry in batch:
            rc = ClassifyAdaptive(dists[i], margin, dynamic_d_k)  // 新方法
            // ... schedule I/O (同现有逻辑)

            // 更新 est_heap_
            update_est_heap(dists[i], entry_id)
```

**注意**: est_heap_ 在 batch 内持续更新（供下一个 cluster 的 dynamic_d_k 使用），但 dynamic_d_k 本身在 cluster 内不变。

#### 4b. 早停替换

```cpp
// 旧:
if (config_.early_stop &&
    ctx.collector().Full() &&
    ctx.collector().TopDistance() < index_.conann().d_k())

// 新:
float est_kth = (est_heap_.size() >= est_top_k_)
    ? est_heap_.front().first
    : std::numeric_limits<float>::infinity();
if (config_.early_stop &&
    crc_stopper_.ShouldStop(i + 1, est_kth))
```

#### 4c. Prefetch 参数化

SearchConfig 变更：
```cpp
struct SearchConfig {
    // ...existing...
    uint32_t probe_batch_size = 128;          // 从 64 改为 128
    uint32_t prefetch_depth = 16;             // max_prefetch 上限（保持）
    uint32_t initial_prefetch = 4;            // 新增：初始预取数
    uint32_t refill_threshold = 2;            // 保持
    uint32_t refill_count = 2;                // 保持

    // CRC 参数
    const CalibrationResults* crc_params = nullptr;  // 新增
};
```

PrefetchClusters 改为提交 `min(initial_prefetch, nlist)` 个初始读取。

#### 4d. Fallback 兼容

当 `config_.crc_params == nullptr` 时，保持原有 d_k 早停逻辑和静态 Classify，不影响现有功能。

### 5. probe_batch_size 改为 128

将默认 `probe_batch_size` 从 64 改为 128。理由：
- 减少 batch 循环的 overhead
- 更好地利用 CPU cache line（128 × sizeof(float) = 512B 距离数组在 L1 内）
- RaBitQ XOR+popcount 本身非常快，larger batch 不会造成 latency 问题

---

## 数据流

```
                    QUERY ENTRY
                         │
                         ▼
    ┌──────────────────────────────────────────┐
    │  PrefetchClusters(initial=4)             │
    │  Submit 4 cluster block reads            │
    └──────────────────────────────────────────┘
                         │
                         ▼
    ┌──────────────────────────────────────────┐
    │  ProbeAndDrainInterleaved                │
    │  for each sorted_cluster[i]:             │
    └──────────────────────────────────────────┘
         │
         ├─ dynamic_d_k = est_heap_.top ?: static_d_k
         │
         ├─ ProbeCluster:
         │   ├─ RaBitQ estimate (batch=128)
         │   ├─ ClassifyAdaptive(dist, margin, dynamic_d_k)
         │   │   ├─ SafeOut: dist > dynamic_d_k + 2m → skip (越来越多)
         │   │   ├─ SafeIn:  dist < static_d_k - 2m  → fetch
         │   │   └─ Uncertain                         → fetch
         │   └─ update est_heap_ (per vector)
         │
         ├─ Submit reads
         │
         ├─ CrcStopper.ShouldStop(i+1, est_heap_.top)?
         │   ├─ YES → break
         │   └─ NO → refill window (if inflight < 2, add 2)
         │
         └─ next cluster...
```

---

## Decisions

### D1: SafeOut 使用 est_heap (RaBitQ) 的 dynamic_d_k
- est_heap 和 CrcStopper 使用相同距离空间（RaBitQ estimate）
- margin 本身设计用于补偿 RaBitQ 误差，已有安全余量

### D2: dynamic_d_k 仅 cluster 间更新
- cluster 内 batch 间不重算 dynamic_d_k
- 简化实现，避免同一 cluster 内 SafeOut 率突变

### D3: CRC 标定使用 RaBitQ 距离
- 保证标定（d_min/d_max/λ̂）和推理（CrcStopper 输入）在同一距离空间
- 需要 CrcCalibrator 接口新增 RaBitQ codes + rotation 参数

### D4: probe_batch_size 从 64 改为 128
- 减少循环开销，更好利用 cache

### D5: Prefetch 参数化
- initial_prefetch=4 避免过度预取
- refill_count=2 维持流水线
- max_prefetch=16 作为上限

### D6: Fallback 兼容
- crc_params == nullptr 时退回 d_k 早停 + 静态 Classify
- 不影响现有测试和功能

---

## Risks / Trade-offs

- **[RaBitQ 距离作为 SafeOut 门槛]** RaBitQ 可能低估距离，导致 dynamic_d_k 偏小。但 SafeOut 条件是 `dist > d_k + 2·margin`，margin 补偿了估计误差。需要通过 benchmark 验证误杀率。
- **[两个 Heap 的内存开销]** est_heap_ 最多 top_k 个 pair<float, uint32_t>，top_k=10 时仅 80 字节，可忽略。
- **[CrcCalibrator 复杂度增加]** 需要传入 RaBitQ codes，但标定是离线操作，可接受。
- **[Prefetch 减少可能影响流水线]** initial_prefetch=4 相比原来的 16 减少了初始预取。如果前 4 个 cluster 的 probe 时间不够覆盖后续 cluster 的 I/O 时间，可能出现 stall。需要 benchmark 验证。
