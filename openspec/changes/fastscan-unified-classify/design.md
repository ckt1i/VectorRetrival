## Context

bench_vector_search 实验数据 (deep1m, dim=96, nlist=4096, nprobe=100, bits=4):
- Popcount 分类: recall@10=0.966, avg=3.14ms, S1 SafeOut=63.65%
- FastScan 分类 (距离误差校准): recall@10=0.965, avg=2.29ms, S1 SafeOut=63.19%
- eps_ip_pop=0.171, eps_ip_fs=0.178 (几乎相同)
- False SafeOut: Popcount=0, FastScan=0

当前 ProbeCluster per-block(32) 计算:
1. FastScan VPSHUFB → dists[32] (已算但不用于分类)
2. Popcount 逐向量循环 → pop_dists[32] (用于分类) ← 要删除的部分
3. S1 分类用 pop_dists
4. S2 ExRaBitQ (Uncertain only)

## Goals / Non-Goals

**Goals:**
- 删除 ProbeCluster 中的 Popcount 循环，用 FastScan dists[] 直接分类
- IvfBuilder 校准改为距离误差方法，确保 margin 正确覆盖 FastScan 距离误差
- CRC calibration 一致性：用 FastScan 距离替代 popcount 距离

**Non-Goals:**
- 不改变 S2 ExRaBitQ 逻辑（仍用 IPExRaBitQ per-Uncertain-vector）
- 不改变 ConANN margin 公式本身（margin = 2·r_max·norm_qc·eps_ip）
- 不改变 FlatBuffers schema（eps_ip 字段语义不变）

## Decisions

### Decision 1: IvfBuilder CalibrateEpsilonIp 改为距离误差校准

当前 `CalibrateEpsilonIp` (ivf_builder.cpp:159) 和 `CalibrateEpsilonIpFastScan` (ivf_builder.cpp:248) 都比较 IP 估计 vs "精确" IP（即 1-bit 量化后的点乘）。这只衡量了 FastScan 路径相对于 Popcount 路径的量化误差，而不是相对于真实 L2 距离的误差。

改为: 对每个 (query, target) 采样对:
```
fs_dist = EstimateDistanceFastScan(query, target)   // FastScan 估计距离
true_dist = L2Sqr(query_raw, target_raw)              // 真实 L2 距离
dist_err = |fs_dist - true_dist|
normalized_err = dist_err / (2 · norm_oc · norm_qc)   // 归一化为 IP 误差单位
```

eps_ip = P99(normalized_err)。这个值 ≈ 0.178，与当前 popcount eps_ip ≈ 0.171 非常接近。

只需修改 `CalibrateEpsilonIp`，删除 `CalibrateEpsilonIpFastScan`（不再需要两套校准）。

### Decision 2: ProbeCluster 删除 Popcount 循环

当前 overlap_scheduler.cpp ProbeCluster 在 FastScan batch-32 之后有一个逐向量 Popcount 循环 (行 224-240)。改为:

```
// 删除: pop_dists 计算
// 直接用 dists[] (FastScan) 做:
for (uint32_t j = 0; j < count; ++j) {
    ResultClass rc = conann.ClassifyAdaptive(dists[j], margin, dynamic_d_k);
    // CRC est_heap 也用 dists[j]
    ...
}
```

同时删除: `UnpackSignBitsFromFastScan`、`PopcountXor` 调用、`pop_dists[32]` 数组、`inv_dim`/`code_words` 局部变量。

### Decision 3: CRC CalibrateWithRaBitQ 改用 FastScan 距离

CRC calibration 需要每个 (query, vector) 对的距离估计来建立 CRC 模型。当前用 popcount 距离。改为用 FastScan 距离，确保校准数据和搜索路径一致。

这需要在 CrcCalibrator 中引入 FastScan 批处理，或者传入预计算的 FastScan 距离。最简方案：在 CrcCalibrator::CalibrateWithRaBitQ 中，使用 EstimateDistanceFastScan 替代 EstimateDistanceRaw。

## Risks / Trade-offs

- **[Risk] 旧索引兼容性**: 旧索引的 eps_ip 是 popcount 校准的，用 FastScan 分类时 margin 可能略窄。但实测两者几乎相同（0.171 vs 0.178），旧索引仍安全。
- **[Risk] CRC 校准一致性**: CRC 模型依赖距离分布。换 FastScan 距离后需要重新校准 CRC。这是正确的——校准和搜索应该用同一种距离估计。
- **[Trade-off] 删除 Popcount 路径后无法回退**: 保留 `--use-fastscan` flag 在 bench_vector_search 中用于 A/B 对比。生产路径只用 FastScan。
