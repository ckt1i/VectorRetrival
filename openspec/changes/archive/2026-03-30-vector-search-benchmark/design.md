# 设计：纯向量搜索 Benchmark

## 概述

新增 fvecs/ivecs IO 库和 `bench_vector_search` benchmark，支持标准 ANN-Bench 数据集的纯向量搜索验证，并在聚类探测循环中接入 CRC early-stop。

## 架构

```
bench_vector_search 流水线
═══════════════════════════════════════════════════════════════════

  CLI 参数
  --base /path/to/base.fvecs    (或 .npy)
  --query /path/to/query.fvecs  (或 .npy)
  --gt /path/to/gt.ivecs        (可选)
  --nlist 256 --nprobe 32 --topk 10
  --crc 1 --crc-alpha 0.1
       │
       ▼
  Phase A: Load
    按扩展名分派:
      .fvecs → LoadFvecs()
      .npy   → LoadNpyFloat32()
      .ivecs → LoadIvecs()    (GT, 可选)
       │
       ▼
  Phase B: Build (纯内存)
    KMeans → RaBitQ Encode → 校准 ε_ip / d_k
       │
       ▼
  Phase C: CRC Calibration (--crc)
    CalibrateWithRaBitQ(内联) → CalibrationResults
    → CrcStopper
       │
       ▼
  Phase D: Vector Search (per query)
    ┌─────────────────────────────────────────────────────┐
    │  sorted = FindNearestClusters(query, centroids)     │
    │                                                     │
    │  est_heap[top_k]   (RaBitQ 估计距离, max-heap)      │
    │  exact_heap[top_k] (精确 L2, max-heap)              │
    │                                                     │
    │  for i = 0..nprobe-1:                               │
    │    cid = sorted[i]                                  │
    │    PrepareQuery(query, centroid[cid])                │
    │    for vec in cluster[cid]:                          │
    │      est_dist = RaBitQ.Estimate(vec)                │
    │      class = ConANN.Classify(est_dist, kth)         │
    │      if SafeIn/Uncertain:                           │
    │        est_heap.push(est_dist, vec_id)              │
    │        exact_dist = L2Sqr(query, raw_vec)           │
    │        exact_heap.push(exact_dist, vec_id)          │
    │      SafeOut → skip rerank                          │
    │                                                     │
    │    // CRC Early Stop                                │
    │    if --crc:                                         │
    │      est_kth = est_heap.top() or +inf               │
    │      if crc_stopper.ShouldStop(i+1, est_kth) break  │
    │    else:                                             │
    │      if exact_heap.full && top < d_k → break        │
    │                                                     │
    │  output: exact_heap → top-K results                 │
    └─────────────────────────────────────────────────────┘
       │
       ▼
  Phase E: Recall vs GT
    GT 来源: .ivecs 文件 或 暴力 L2 自算
    recall@1, @5, @10, @100
    latency (avg/p50/p95/p99)
    ConANN stats (SafeIn/SafeOut/Uncertain 比例)
    CRC stats (avg_probed, early_stop_rate)
       │
       ▼
  Phase F: Output (terminal + JSON)
```

## 数据模型

### fvecs/ivecs 格式

标准 ANN-Benchmark 二进制格式，每条记录：

```
┌──────────┬──────────────────────────────┐
│ dim: i32 │ values: float[dim] 或 i32[dim] │
└──────────┴──────────────────────────────┘
```

每条记录 `4 + dim * 4` 字节。所有记录的 dim 必须相同。

### IO API

```cpp
namespace vdb::io {

// 复用现有类型
StatusOr<NpyArrayFloat> LoadFvecs(const std::string& path);

// 新类型 (GT 需要 int32)
struct VecsArrayInt32 {
    std::vector<int32_t> data;   // row-major [rows × cols]
    uint32_t rows;
    uint32_t cols;
};

StatusOr<VecsArrayInt32> LoadIvecs(const std::string& path);

}  // namespace vdb::io
```

复用 `NpyArrayFloat` 避免引入冗余类型。GT 使用 `VecsArrayInt32` 因为 ivecs 存的是 int32。

### 扩展名探测

```cpp
// 按扩展名自动选择 reader
StatusOr<NpyArrayFloat> LoadVectors(const std::string& path) {
    if (EndsWith(path, ".fvecs")) return LoadFvecs(path);
    if (EndsWith(path, ".npy"))   return LoadNpyFloat32(path);
    return Status::InvalidArgument("Unsupported format: " + path);
}
```

bench_vector_search 的 `--base` 和 `--query` 参数使用此函数，无需额外 `--format` 参数。

### GT 处理

```
--gt 指定时:
  扩展名 .ivecs → LoadIvecs()
  扩展名 .npy   → LoadNpyFloat32() → cast to int
--gt 未指定或文件不存在:
  暴力 L2 计算 GT (N × Q × L2Sqr)
  ⚠ 1M 级别较慢 (~30s)，打印警告
```

### CRC 接入

内联计算路径（不依赖 crc_scores.bin）：

```
if --crc:
  1. 构建 ClusterData[] (内存中已有 codes + ids)
  2. CalibrateWithRaBitQ(config, queries, centroids, clusters, rotation)
     → CalibrationResults
  3. CrcStopper stopper(calib_results, nlist)
  4. 在 probe 循环中: stopper.ShouldStop(probed_count, est_kth)
```

这与 OverlapScheduler 中的逻辑一致，但不经过 io_uring。

## CLI 参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--base` | (必需) | base 向量文件路径 (.fvecs/.npy) |
| `--query` | (必需) | query 向量文件路径 (.fvecs/.npy) |
| `--gt` | "" | GT 文件路径 (.ivecs)，空则自算 |
| `--nlist` | 256 | KMeans 聚类数 |
| `--nprobe` | 32 | 探测聚类数 |
| `--topk` | 10 | top-K |
| `--crc` | 0 | 启用 CRC early-stop |
| `--crc-alpha` | 0.1 | CRC α 参数 |
| `--crc-calib` | 0.5 | CRC 标定集比例 |
| `--crc-tune` | 0.1 | CRC 调优集比例 |
| `--early-stop` | 1 | 启用 d_k early-stop |
| `--seed` | 42 | 随机种子 |
| `--outdir` | "" | JSON 输出目录（空则不输出） |
| `--queries` | 0 | 限制 query 数量（0=全部） |
| `--p-for-dk` | 90 | d_k 校准百分位 |
| `--p-for-eps` | 95 | ε_ip 校准百分位 |

## 现有 Benchmark 适配

`bench_rabitq_accuracy` 和 `bench_rabitq_diagnostic` 当前硬编码 `--dataset` 目录下的 npy 文件名。改造方案：

```
改造前:
  --dataset /path/to/coco_1k
    → data_dir + "/image_embeddings.npy"   (硬编码)
    → data_dir + "/query_embeddings.npy"   (硬编码)

改造后:
  --base /path/to/base.fvecs    (或 .npy，新增)
  --query /path/to/query.fvecs  (或 .npy，新增)
  --dataset /path/to/coco_1k    (保留，作为回退)

  优先级: --base/--query > --dataset
  回退:   --base 未指定时 = {dataset}/image_embeddings.npy
          --query 未指定时 = {dataset}/query_embeddings.npy
```

加载逻辑统一使用 `LoadVectors(path)`，按扩展名自动分派。两个 benchmark 后续的 KMeans、RaBitQ 编码、精度分析逻辑完全不变。

## 决策记录

| ID | 决策 | 理由 |
|----|------|------|
| D1 | CLI 参数指定文件路径 + 扩展名探测 | 最灵活，不依赖文件命名约定 |
| D2 | GT 按存在性判断：有 .ivecs 则加载，无则自算 | 兼容有 GT 和无 GT 的数据集 |
| D3 | CRC 走内联计算 | 纯内存 benchmark，所有数据已在内存中，无需 crc_scores.bin |
| D4 | fvecs 返回 `NpyArrayFloat` | 复用类型，base/query 加载函数统一返回类型 |
| D5 | ivecs 用新类型 `VecsArrayInt32` | GT 是 int32，不同于 float 向量 |
| D6 | Probe 循环维护 est_heap + exact_heap | est_heap 供 CRC ShouldStop 使用，exact_heap 出最终结果 |
| D7 | 现有 benchmark 保留 `--dataset` 回退 | 向后兼容，COCO 数据集仍可用 `--dataset` 一个参数搞定 |
