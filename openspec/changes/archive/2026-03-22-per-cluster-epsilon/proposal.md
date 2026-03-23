# Proposal: Per-Cluster Epsilon via Reconstruction Error

## What

将 ConANN 的全局理论 epsilon（`c_factor · 2^(-B/2) / √dim`）替换为基于量化重建误差的 per-cluster epsilon。在 build 阶段，对每个 cluster 计算所有向量的 RaBitQ 重建误差（`L2(v, v̂)`），取 P95 作为该 cluster 的 epsilon，存储在 Lookup Table Entry 中。查询时，ProbeCluster 使用 per-cluster epsilon 构造局部 ConANN 分类器。

## Why

- **理论 epsilon 不匹配实际数据**: benchmark 显示 coco_1k 上全局 epsilon 极小（d_k=0.8236, tau_out=1.1830），而 100% 的 RaBitQ 估计距离 > tau_out，导致所有向量被 SafeOut，分类完全失效
- **理论公式假设不成立**: `ε = c · 2^(-B/2) / √D` 是对 uniform random vectors 的渐近界，对 real-world embeddings 分布不适用
- **Cluster 间量化质量差异大**: 不同 cluster 的向量分布不同，量化误差差异显著，全局 epsilon 无法同时适配所有 cluster

## Scope

### In Scope
- `ClusterLookupEntry` 新增 `float epsilon` 字段
- `IvfBuilder`: build 时计算每个 cluster 的 RaBitQ 重建误差 P95 作为 epsilon
- 采样策略: 若 cluster 向量数 < 采样数的 2 倍，则采样数 = max(cluster_size / 2, 1)
- `ClusterStoreWriter/Reader`: 写入/读取 per-cluster epsilon
- `ParsedCluster` 新增 `float epsilon` 字段
- `ProbeCluster`: 使用 per-cluster epsilon 构造局部 ConANN
- `bench_rabitq_accuracy`: 输出各 cluster 的聚类数和 epsilon 值
- 移除 `RaBitQConfig::c_factor` 和 `ConANN::FromConfig` 的理论 epsilon 路径

### Out of Scope
- 动态 d_k（仍使用全局 calibrated d_k）
- Per-query adaptive epsilon
- 多 bit RaBitQ 支持

## Decisions

- **存储方案 A**: epsilon 存入 Lookup Table Entry（而非 data block 或单独文件），只增加 4 bytes/cluster
- **标定算法**: 纯重建误差 `L2(v, v̂)`，不需要 query 采样
- **重建公式**: `v̂ = centroid + norm · P × (signs/√dim)` → `error = L2(v, v̂)`
- **百分位**: P95（而非 P99），平衡 SafeOut 安全性和 Uncertain 比例
- **采样策略**: 每 cluster 采样 `n_samples` 个向量计算误差；若 cluster_size < 2 × n_samples，则 n_samples = max(cluster_size / 2, 1)
