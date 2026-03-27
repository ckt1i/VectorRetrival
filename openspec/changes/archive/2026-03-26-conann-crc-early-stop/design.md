## Context

VDB 当前的早停逻辑 (`overlap_scheduler.cpp:258-265`) 基于 `d_k` 阈值：`TopDistance() < d_k` 时停止 probe。这是一个固定阈值的启发式方法，无法提供 FNR 保证。

ConANN 论文提出的 Conformal Risk Control (CRC) 框架通过离线标定 + 在线自适应判断，在统计意义上保证 FNR ≤ α。本设计将 CRC 引入 VDB，分两阶段：
1. Phase 1: Benchmark 验证（自包含，不走 OverlapScheduler，不用 RaBitQ）
2. Phase 2: 集成到 OverlapScheduler，替换 d_k 早停

## Goals / Non-Goals

**Goals:**
- 实现完整的 CRC 离线标定流程
- 实现 CRC 在线早停判断
- Phase 1 benchmark 验证 CRC 在 coco 数据集上的剪枝效果和 recall 保证
- Phase 2 集成到 OverlapScheduler 替换 d_k 早停

**Non-Goals:**
- 修改 RaBitQ 三分类逻辑（保持 SafeIn/SafeOut/Uncertain 不变）
- 从 λ̂ 反推 d_k（直接用 λ̂ 判断，不需要中间转换）
- 支持 GPU 加速标定

---

## 离线标定流程 (CrcCalibrator)

### 总览

```
输入:
  Q_cal       校准查询集 [n_queries × dim]
  centroids   聚类质心 [nlist × dim]
  clusters    每个聚类的向量数据 (ID + 原始向量)
  GT          每条查询的 ground truth top-k 邻居 ID [n_queries × k]
  α           目标 FNR 上界 (如 0.1)
  k           top-k

输出:
  CalibrationResults { λ̂, k_reg, λ_reg, d_min, d_max }
```

```
┌───────────────────────────────────────────────────────────────────┐
│                        离线标定流程                                │
├───────────────────────────────────────────────────────────────────┤
│                                                                   │
│  Q_cal ──┬── calib (50%) ──→ compute_scores ──→ normalize ──┐    │
│          │                                                   │    │
│          ├── tune  (10%) ──→ compute_scores ──→ normalize    │    │
│          │       │                                           │    │
│          │       └──→ pick_lambda_reg ◄──────────────────────┘    │
│          │                 │                                      │
│          │           (k_reg, λ_reg)                               │
│          │                 │                                      │
│          │                 ▼                                      │
│          │           Brent 求根 (calib 集)                        │
│          │                 │                                      │
│          │                 ▼                                      │
│          │               λ̂                                       │
│          │                                                        │
│          └── test  (40%) ──→ evaluate (验证 FNR ≤ α)             │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

### Step 0: 划分校准集

将 Q_cal 随机打乱后按比例划分：

| 集合   | 比例 | 用途                                 |
|--------|------|--------------------------------------|
| calib  | 50%  | 计算 nonconformity score + Brent 求 λ̂ |
| tune   | 10%  | 选择最优 (k_reg, λ_reg)              |
| test   | 40%  | 最终评估 FNR 和平均 probe 数          |

### Step 1: compute_scores — 全量增量搜索

对 calib 和 tune 集中的每条查询 `q` 执行如下操作：

```
输入: 查询向量 q, 所有聚类质心 centroids, 所有聚类向量数据 clusters
输出: nonconf_scores[nlist], predictions[nlist] (每个 nprobe 步的 score 和 top-k 预测集)

1. 计算 q 到每个聚类质心的距离，排序得到 sorted_cluster_ids[0..nlist-1]
2. 初始化 top_k_heap (最大堆, 容量 k)
3. for p = 0, 1, ..., nlist-1:
     a. cid = sorted_cluster_ids[p]
     b. 遍历 cluster[cid] 中的每个向量 v:
          dist = L2(q, v)                  // 精确 L2 距离
          if heap.size() < k:
              heap.push(v.id, dist)
          elif dist < heap.top().dist:
              heap.pop()
              heap.push(v.id, dist)
     c. d_p = heap.top().dist              // 当前第 k 近距离 (堆顶)
        (若堆未满, d_p = +∞)
     d. 记录 raw_scores[q][p] = d_p        // 原始距离，后续归一化
     e. 记录 predictions[q][p] = heap 中所有 ID 的集合
```

**关键点**:
- 这是增量搜索：每步只 probe 一个新聚类，堆在步骤间持续维护
- 记录的是每步的原始距离 `d_p`，归一化在下一步统一做
- `predictions[q][p]` 记录 probe 了 p+1 个聚类后的 top-k 预测集，用于后续计算 FNR
- 复杂度: O(|Q| × nlist × avg_cluster_size)，对 coco-1k (1000 queries × ~100 clusters × ~10 vecs/cluster) 约 10^6 次距离计算

### Step 2: 全局归一化

从 calib 集（不含 tune 和 test）的所有 `raw_scores` 中统计：

```
d_min = min{ raw_scores[q][p] | q ∈ calib, p ∈ [0, nlist-1], heap 已满 }
d_max = max{ raw_scores[q][p] | q ∈ calib, p ∈ [0, nlist-1], heap 已满 }
```

注意：只统计堆已满（`heap.size() == k`）时的 `d_p`。堆未满时 `d_p = +∞`，不参与统计。

然后对 calib 和 tune 集中的所有 raw_scores 做归一化：

```
                     d_p(q) − d_min
nonconf(q, p) = ─────────────────────
                     d_max − d_min

nonconf(q, p) = clamp(nonconf(q, p), 0, 1)
```

**语义**:
- `nonconf = 0`: 当前 top-k 质量最好（d_p 等于全局最小值）
- `nonconf = 1`: 当前 top-k 质量最差（d_p 等于全局最大值）
- 堆未满时: `d_p = +∞`，clamp 后 `nonconf = 1.0`

```
    d_max ─────────────── nonconf = 1.0 (最差, top-k 质量低)
      │
      │   d_p (当前 top-k 的第 k 近距离)
      │     ↓
      │   nonconf = (d_p - d_min) / (d_max - d_min)
      │
    d_min ─────────────── nonconf = 0.0 (最优, top-k 质量高)
```

### Step 3: RAPS 正则化

RAPS (Regularized Adaptive Prediction Sets) 在 nonconformity score 的基础上加入 rank 惩罚项，使停止决策更均匀。

对每条查询 `q` 在 nprobe 步 `p`（rank = p+1，从 1 开始计）：

```
E(q, p) = (1 − nonconf(q, p)) + λ_reg · max(0, (p+1) − k_reg)

max_reg_val = 1 + λ_reg · max(0, nlist − k_reg)

reg_score(q, p) = E(q, p) / max_reg_val
```

**各项含义**:

| 项 | 含义 |
|----|------|
| `1 - nonconf(q, p)` | 当前 top-k 质量的"置信度"。nonconf 越小（质量越好），此项越大 |
| `λ_reg · max(0, (p+1) - k_reg)` | rank 惩罚项。probe 超过 k_reg 个聚类后，每多 probe 一个就加 λ_reg 的惩罚 |
| `max_reg_val` | 归一化因子，确保 `reg_score ∈ [0, 1]` |

**直觉**:
- `reg_score` 越大 → 当前 top-k 置信度高 + 已经 probe 了很多聚类 → 应该停止
- `reg_score` 越小 → 当前 top-k 置信度低 + 还没 probe 多少聚类 → 应该继续
- `k_reg` 相当于一个"免费 probe 额度"：前 k_reg 个聚类不受惩罚
- `λ_reg` 控制惩罚力度：越大则超额 probe 的代价越高

### Step 4: 选择 (k_reg, λ_reg) — 在 tune 集上

遍历候选参数组合：

```
λ_reg ∈ {0, 0.001, 0.01, 0.1}
k_reg ∈ {1, 2, 3, ..., nlist/10}    // 论文中的典型范围
```

对每组 (k_reg, λ_reg)：
1. 用 calib 集的 nonconformity scores 做 RAPS 正则化
2. 在 calib 集上用 Brent 求根找到 λ̂ 使得 `FNR(λ̂) = α`
3. 用这个 λ̂ 在 **tune 集**上计算平均 probe 聚类数（即效率指标）

选择使 **tune 集平均 probe 数最少** 的那组参数。

```
  平均 probe 数
  nlist ┤
        │  ×
        │    ×
        │      × ×
        │          × ←── 选这个 (最低点)
        │            ×
        │
    0   ┤────────────────────
        (k_reg, λ_reg) 组合 →
```

### Step 5: Brent 求根找 λ̂ — 在 calib 集上

用 Step 4 选定的 (k_reg, λ_reg)，在 calib 集上最终求解 λ̂：

```
目标: 找 λ̂ ∈ [0, 1] 使得 FNR(λ̂) = α
```

**FNR(λ) 的计算过程**:

```
对 calib 集中每条查询 q:
  1. 确定停止点: 从 p = 0 开始遍历 reg_score[q][p]
     找到第一个 p_stop 使得 reg_score[q][p_stop] > λ
     (如果没有，p_stop = nlist - 1)

  2. 获取停止时的预测集: pred_set = predictions[q][p_stop]

  3. 判断是否漏检:
     if GT[q] ⊄ pred_set:   // GT 中存在不在预测集中的 ID
         FN += 1

FNR(λ) = FN / |calib|
```

**Brent 求根**:

```
f(λ) = FNR(λ) - α

性质:
  f(0) = FNR(0) - α ≈ 0 - α < 0     // λ=0: 几乎不早停, FNR ≈ 0
  f(1) = FNR(1) - α ≈ 1 - α > 0     // λ=1: 全部早停, FNR ≈ 1
  f 关于 λ 单调递增 (λ 越大 → 越容易早停 → FNR 越高)

使用 GSL gsl_root_fsolver_brent 在 [0, 1] 上求根:
  tolerance: 1e-6
  最大迭代: 100
```

```
  FNR(λ)
  1.0 ┤╲
      │  ╲
      │   ╲
  α ──│────╳──── ← λ̂
      │     ╲
      │      ╲
  0.0 ┤───────╲──
      0       λ̂     1.0
               λ →
```

### Step 6: evaluate_test — 在 test 集上验证

用最终的 CalibrationResults 在 test 集上评估：

```
对 test 集中每条查询 q:
  1. 对 q 执行 compute_scores (增量搜索)
  2. 归一化 + RAPS 正则化 (使用已标定的 d_min, d_max, k_reg, λ_reg)
  3. 找到停止点 p_stop (reg_score > λ̂ 的第一个 p)
  4. 记录: probed_clusters[q] = p_stop + 1
  5. 记录: is_fn[q] = (GT[q] ⊄ predictions[q][p_stop])

输出:
  actual_FNR = count(is_fn) / |test|     // 应 ≤ α
  avg_probed = mean(probed_clusters)      // 越小越好
  recall@1, recall@5, recall@10          // 各 recall 指标
```

### 完整数据结构

```cpp
struct CalibrationResults {
    float lamhat;        // λ̂: Brent 求根得到的停止阈值
    uint32_t kreg;       // k_reg: RAPS 免费 probe 额度
    float reg_lambda;    // λ_reg: RAPS rank 惩罚系数
    float d_min;         // 全局最小 d_p (归一化下界)
    float d_max;         // 全局最大 d_p (归一化上界)
};
```

标定中间数据（不持久化，仅标定过程使用）:

```cpp
// 每条查询在每个 nprobe 步的原始距离
// raw_scores[q][p] = probe 了 p+1 个聚类后的 top-k 第 k 近距离
std::vector<std::vector<float>> raw_scores;        // [n_queries × nlist]

// 归一化后的 nonconformity scores
std::vector<std::vector<float>> nonconf_scores;    // [n_queries × nlist]

// RAPS 正则化后的 scores
std::vector<std::vector<float>> reg_scores;        // [n_queries × nlist]

// 每条查询在每个 nprobe 步的 top-k 预测集
std::vector<std::vector<std::vector<idx_t>>> predictions;  // [n_queries × nlist × k]
```

---

## 在线早停流程 (CrcStopper)

### 总览

```
输入:
  CalibrationResults { λ̂, k_reg, λ_reg, d_min, d_max }
  nlist (聚类总数, 用于预计算 max_reg_val)

每次 probe 完一个聚类后:
  输入: probed_count (已 probe 的聚类数, 从 1 开始)
        current_kth_dist (当前 top-k 堆的第 k 近距离, 堆未满时为 +∞)
  输出: bool (true = 停止, false = 继续)
```

### 判断逻辑

```
Step 1: 归一化
  nonconf = (current_kth_dist - d_min) / (d_max - d_min)
  nonconf = clamp(nonconf, 0.0, 1.0)

Step 2: RAPS 正则化
  E = (1 - nonconf) + λ_reg · max(0, probed_count - k_reg)
  reg_score = E / max_reg_val

  其中 max_reg_val = 1 + λ_reg · max(0, nlist - k_reg)  // 构造时预计算

Step 3: 与 λ̂ 比较
  return reg_score > λ̂
```

**O(1) 操作**: 无堆分配、无循环、无分支预测开销。

### 直觉解释

```
                    reg_score 的两个分量
  ────────────────────────────────────────────────────────

  分量 1: (1 - nonconf) — "top-k 质量置信度"

    nonconf = 0   →  1 - nonconf = 1  →  质量好, 倾向停止
    nonconf = 1   →  1 - nonconf = 0  →  质量差, 倾向继续

  分量 2: λ_reg · max(0, p - k_reg) — "已 probe 过多的惩罚"

    p ≤ k_reg   →  惩罚 = 0      →  还在免费额度内
    p > k_reg   →  惩罚线性增长   →  probe 越多越倾向停止

  两个分量相加, 归一化后与 λ̂ 比较:
    reg_score > λ̂  →  停止  (质量够好 和/或 已经 probe 够多)
    reg_score ≤ λ̂  →  继续
```

### 边界情况

| 场景 | current_kth_dist | nonconf | reg_score | 行为 |
|------|------------------|---------|-----------|------|
| 堆未满 | +∞ | 1.0 (clamp) | 0 + reg_term (很小) | 不停止 ✓ |
| 已找到最优 | d_min | 0.0 | 1/max_reg_val + reg_term | 很快停止 ✓ |
| d_max == d_min | 任意 | 设为 0.0 | 1/max_reg_val + reg_term | 视为最优 |
| p == nlist | 任意 | 任意 | 任意 | 外层循环自然结束，不需判断 |

### CrcStopper 接口

```cpp
class CrcStopper {
public:
    /// @param params   离线标定结果
    /// @param nlist    聚类总数 (用于预计算 max_reg_val)
    CrcStopper(const CalibrationResults& params, uint32_t nlist);

    /// 每 probe 完一个聚类后调用
    /// @param probed_count     已 probe 的聚类数 (从 1 开始)
    /// @param current_kth_dist 当前 top-k 堆的第 k 近距离 (堆未满时传 +∞)
    /// @return true = 应该停止
    bool ShouldStop(uint32_t probed_count, float current_kth_dist) const;

private:
    float lamhat_;
    uint32_t kreg_;
    float reg_lambda_;
    float d_min_;
    float d_range_inv_;   // 1.0 / (d_max - d_min), 预计算避免除法
    float max_reg_val_;   // 1 + reg_lambda * max(0, nlist - kreg), 预计算
};
```

---

## 两个集成面

### Phase 1: bench_conann_crc (自包含 Benchmark)

```
┌──────────────────────────────────────────────────────────────┐
│  bench_conann_crc                                            │
│                                                              │
│  1. 加载 coco 数据 (img.npy, qry.npy)                       │
│  2. KMeans 聚类 → centroids + cluster assignments           │
│  3. 计算 ground truth (brute-force top-k)                   │
│  4. CrcCalibrator::Calibrate()                               │
│     ├── split calib/tune/test                                │
│     ├── compute_scores (精确 L2)                             │
│     ├── normalize (d_min, d_max)                             │
│     ├── pick_lambda_reg (tune 集)                            │
│     └── brent_solve (calib 集) → λ̂                          │
│  5. evaluate_test() → 报告 FNR, avg_probed, recall          │
│  6. 扫描多个 α 值对比                                        │
│                                                              │
│  不走 OverlapScheduler, 不用 RaBitQ                          │
│  纯粹验证 CRC 算法在 coco 上的剪枝效果                       │
└──────────────────────────────────────────────────────────────┘
```

Benchmark 的核心循环（概念）：

```
for each query q in test_set:
    sorted_clusters = sort clusters by dist(q, centroid)
    top_k_heap = {}
    for p = 0; p < nlist; p++:
        // probe cluster
        for each vec in cluster[sorted_clusters[p]]:
            dist = L2(q, vec)
            heap_update(top_k_heap, vec.id, dist)

        // CRC 早停判断
        if crc_stopper.ShouldStop(p + 1, top_k_heap.top().dist):
            break

    // 记录结果
    record recall, probed_count
```

### Phase 2: OverlapScheduler 集成

替换 `overlap_scheduler.cpp:258-265`:

```cpp
// ═══════════════════════════════════════════════════
// 当前代码 (d_k 早停)
// ═══════════════════════════════════════════════════
if (config_.early_stop &&
    ctx.collector().Full() &&
    ctx.collector().TopDistance() < index_.conann().d_k()) {
    ctx.stats().early_stopped = true;
    ctx.stats().clusters_skipped =
        static_cast<uint32_t>(sorted_clusters.size() - 1 - i);
    break;
}

// ═══════════════════════════════════════════════════
// 替换为 (CRC 早停)
// ═══════════════════════════════════════════════════
if (config_.early_stop &&
    crc_stopper_.ShouldStop(i + 1, ctx.collector().TopDistance())) {
    ctx.stats().early_stopped = true;
    ctx.stats().clusters_skipped =
        static_cast<uint32_t>(sorted_clusters.size() - 1 - i);
    break;
}
```

**注意**: CrcStopper 内部处理堆未满的情况（`TopDistance = +∞` 时 nonconf=1.0，reg_score 很小，不触发早停），因此不需要外部 `collector().Full()` 守卫。

**CalibrationResults 的传递路径**:

```
IvfIndex (持有 CalibrationResults, build 时标定)
    │
    ▼
OverlapScheduler 构造时:
    if (index_.has_crc_params())
        crc_stopper_ = CrcStopper(index_.crc_params(), index_.nlist());
    │
    ▼
ProbeAndDrainInterleaved() 中每次 probe 后调用 crc_stopper_.ShouldStop()
```

**与 RaBitQ 三分类的关系**:

```
两层剪枝正交运作:

聚类级: CRC 早停 (决定是否继续 probe 下一个聚类)
  │
  ▼ (每个被 probe 的聚类内部)
向量级: RaBitQ 三分类 (决定每个向量是 SafeIn/SafeOut/Uncertain)
  │
  ├── SafeIn:   直接加入 top-k, 跳过精确距离计算
  ├── SafeOut:  直接跳过
  └── Uncertain: 读取原始向量计算精确距离
```

---

## Decisions

### 1. Nonconformity Score 使用论文原始公式（非简化形式）

```
nonconf(q, p) = clamp((d_p(q) - d_min) / (d_max - d_min), 0, 1)
```

**理由**: 原始代码使用 `d_p / MAX_DISTANCE` 是因为其数据集 d_min ≈ 0 的简化。coco 等跨模态数据集 d_min 不一定为 0，必须用完整公式。

### 2. Brent 求根使用 GSL

使用 GSL 库的 `gsl_root_fsolver_brent`，在 [0, 1] 上求解 `FNR(λ) - α = 0`。与原始 ConANN 代码一致。

**依赖**: `sudo apt install libgsl-dev`，CMake 中 `find_package(GSL REQUIRED)` + `target_link_libraries(... GSL::gsl GSL::gslcblas)`。

### 3. d_min, d_max 从 calib 集全局统计

统计 calib 集中所有查询、所有 nprobe 步（堆已满时）的 d_p 的全局最小值和最大值。

### 4. 直接用 λ̂ 判断，不反推 d_k

CRC 聚类级早停和 RaBitQ 向量级三分类完全解耦。CRC 输出的是 `reg_score > λ̂` 的 bool 判断，不需要从 λ̂ 反推出 d_k 来做向量级分类。

---

## Risks / Trade-offs

- **[标定开销]** compute_scores 需要对每条 calib 查询遍历所有 nlist 个聚类，O(n_queries × nlist × avg_cluster_size)。对 coco-1k 可接受，大数据集可能需要采样。
- **[d_min/d_max 泛化]** 全局统计可能对 out-of-distribution 查询不够鲁棒。但作为起步方案足够，后续可考虑 per-query 或 percentile 归一化。
- **[Brent 收敛]** FNR(λ) 理论上单调，但离散查询集上可能有平台区。设置合理的 tolerance (1e-6) 和最大迭代次数 (100) 可控。
- **[top-k 堆未满时]** probe 前几个聚类时堆可能未满，d_p = +∞。此时 nonconf = 1.0，reg_score 最小，不会触发早停。行为正确。
- **[GSL 依赖]** 仅在标定阶段使用 GSL（离线），在线查询不依赖。CrcStopper 的 ShouldStop() 是纯计算，无外部依赖。
