# Design: Per-Cluster Epsilon via Reconstruction Error

## Architecture

```
  Build 阶段 — 每个 Cluster 的 Epsilon 标定流程
  ══════════════════════════════════════════════════════════

  对 cluster k 中的（采样）向量:

  原始向量 v ──encode──▶ code_bits + norm
                              │
                              ▼
                ┌─────────────────────────────┐
                │  Reconstruct:               │
                │  signs[i] = (2·bit[i]-1)/√D │
                │  ō = P × signs              │
                │  v̂ = centroid + norm · ō    │
                └─────────────────────────────┘
                              │
                              ▼
                   error = L2(v, v̂)
                              │
                              ▼
                ┌─────────────────────────────┐
                │  收集所有 error              │
                │  sort → 取 P95              │
                │  → cluster_k.epsilon        │
                └─────────────────────────────┘


  查询阶段 — Per-Cluster ConANN
  ══════════════════════════════════════════════════════════

  ProbeCluster(cluster_k):
    epsilon = pc.epsilon                    ← per-cluster
    d_k     = index_.conann().d_k()         ← global
    tau_in  = d_k - 2·epsilon
    tau_out = d_k + 2·epsilon

    for each vector:
      dist = EstimateDistanceRaw(...)
      if dist < tau_in   → SafeIn
      if dist > tau_out  → SafeOut
      else               → Uncertain
```

## 存储格式

### Lookup Table Entry（方案 A）

```
  ClusterLookupEntry — 磁盘/内存布局
  ════════════════════════════════════

  ┌──────────────┬──────────────┬──────────────┐
  │ cluster_id   │ num_records  │   epsilon     │  ← 新增 4 bytes
  │  uint32      │  uint32      │   float32     │
  ├──────────────┴──────────────┴──────────────┤
  │ centroid[dim]  (dim × float32)             │
  ├────────────────────────────────────────────┤
  │ block_offset (uint64)                      │
  │ block_size   (uint64)                      │
  └────────────────────────────────────────────┘

  影响: 仅增加 4 bytes/cluster，不影响 data block 格式
  文件版本: 仍然是 v5（格式兼容，Lookup Table 结构变化通过 version 隐含）
```

### 版本策略

v5 已经含有 norm+sum_x，本次只在 Lookup Table 增加 epsilon 字段。
将 version 升至 6：
- v5: 无 per-cluster epsilon（全局 c_factor）
- v6: Lookup Table Entry 含 epsilon
- Reader 只接受 v6（不兼容旧版本）

## 重建误差计算

### 数学推导

```
  编码过程:
    r = v - c                       (centroid residual)
    ō = r / ‖r‖                     (normalize)
    ō' = P^T × ō                   (rotate to canonical)
    bits[i] = sign(ō'[i])          (1-bit quantize)

  重建过程:
    signs[i] = (2·bits[i] - 1) / √D  (quantized unit vector in rotated space)
    ō_recon = P × signs               (inverse rotate back to original space)
    v̂ = c + ‖r‖ · ō_recon           (denormalize + add centroid)

  重建误差:
    L2(v, v̂) = ‖v - v̂‖
             = ‖r - ‖r‖ · ō_recon‖
             = ‖r‖ · ‖ō - ō_recon‖         (因为 ‖r‖ = norm)
             = norm · ‖ō - P × (signs/√D)‖

  由正交性 ‖Px‖ = ‖x‖:
    = norm · ‖ō' - signs/√D‖
    = norm · √(2(1 - ⟨ō', signs/√D⟩))

  其中 ⟨ō', signs/√D⟩ = (1/√D) · Σ ō'[i] · sign[i]
```

### 实现策略

在 IvfBuilder 中，编码后直接进行重建（build 时有 rotation matrix）：

```cpp
float ComputeReconstructionError(
    const float* original,        // 原始向量 v
    const float* centroid,        // cluster centroid c
    const RaBitQCode& code,       // 编码结果 (bits + norm)
    const RotationMatrix& rotation,
    Dim dim) {

    const float inv_sqrt_dim = 1.0f / std::sqrt(static_cast<float>(dim));

    // Step 1: bits → ±1/√D (rotated space)
    std::vector<float> sign_vec(dim);
    for (size_t i = 0; i < dim; ++i) {
        int bit = (code.code[i / 64] >> (i % 64)) & 1;
        sign_vec[i] = (2.0f * bit - 1.0f) * inv_sqrt_dim;
    }

    // Step 2: inverse rotate → original space
    std::vector<float> recon_normalized(dim);
    rotation.ApplyInverse(sign_vec.data(), recon_normalized.data());

    // Step 3: v̂ = centroid + norm * recon_normalized
    // Step 4: error² = Σ(v[i] - v̂[i])²
    float err_sq = 0.0f;
    for (size_t i = 0; i < dim; ++i) {
        float diff = original[i] - (centroid[i] + code.norm * recon_normalized[i]);
        err_sq += diff * diff;
    }
    return std::sqrt(err_sq);
}
```

### 采样策略

```
  n_samples_target = 配置值 (如 20)

  对每个 cluster k (cluster_size = |C_k|):
    if cluster_size < 2 × n_samples_target:
        n_samples = max(cluster_size / 2, 1)
    else:
        n_samples = n_samples_target

    随机采样 n_samples 个向量 → 计算重建误差 → sort → P95

  n_samples 取所有 cluster 中计算出的最大值作为最终采样数
  ← 即: global_n_samples = max over all clusters of min(n_samples_target, adjusted)

  注意: 这里 "取最大值" 是全局确定后统一用于所有 cluster 的采样数
```

## 查询路径变更

### ConANN 变更

ConANN 本身不变（仍接受 epsilon + d_k 构造），但 ProbeCluster 不再使用全局 `index_.conann()` 做分类：

```cpp
// Before (全局 ConANN):
const auto& conann = index_.conann();
ResultClass rc = conann.Classify(dists[i]);

// After (per-cluster ConANN):
ConANN cluster_conann(pc.epsilon, index_.conann().d_k());
ResultClass rc = cluster_conann.Classify(dists[i]);
```

### ParsedCluster 扩展

```cpp
struct ParsedCluster {
    std::unique_ptr<uint8_t[]> block_buf;
    const uint8_t* codes_start = nullptr;
    uint32_t code_entry_size = 0;
    uint32_t num_records = 0;
    float epsilon = 0.0f;              // ← 新增
    std::vector<AddressEntry> decoded_addresses;
};
```

epsilon 在 `ParseClusterBlock()` 中从 lookup table entry 复制过来。

## Benchmark 扩展

在 `bench_rabitq_accuracy.cpp` 中新增输出：

```
  Per-Cluster Statistics:
  ╔══════════╤══════════════╤═══════════════╗
  ║ Cluster  │ Num Vectors  │   Epsilon     ║
  ╠══════════╪══════════════╪═══════════════╣
  ║    0     │     63       │   0.0423      ║
  ║    1     │     58       │   0.0312      ║
  ║    2     │     71       │   0.0567      ║
  ║   ...    │    ...       │    ...        ║
  ╚══════════╧══════════════╧═══════════════╝
```

## Changes Summary

| 文件 | 变更 |
|------|------|
| `include/vdb/storage/cluster_store.h` | `ClusterLookupEntry` +epsilon; version 5→6 |
| `src/storage/cluster_store.cpp` | Writer: 写 epsilon; Reader: 读 epsilon, 只接受 v6 |
| `include/vdb/query/parsed_cluster.h` | +`float epsilon` |
| `src/index/ivf_builder.cpp` | 编码后计算重建误差 P95 → 作为 cluster epsilon |
| `src/query/overlap_scheduler.cpp` | 用 per-cluster epsilon 替换全局 conann |
| `tests/index/bench_rabitq_accuracy.cpp` | 输出 per-cluster size + epsilon |
| `include/vdb/common/types.h` | 移除 `c_factor`（可选，或保留但不用于 epsilon） |
