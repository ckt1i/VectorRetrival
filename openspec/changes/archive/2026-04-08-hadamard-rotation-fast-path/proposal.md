# Hadamard 旋转快速路径

## 概述

启用 RaBitQ 的 Walsh-Hadamard 旋转快速路径（已存在但未被 benchmark 使用），将 `RotationMatrix::Apply` 从 O(L²) 矩阵乘降低为 O(L log L) FWHT。在 COCO-100k (dim=512) 上实测延迟从 7.47ms 降到 4.43ms（**-41%**），recall 不变。同时为非 2 的幂维度（如 deep1m 的 dim=96）提供 padding 方案，使其也能受益。最后用 AVX-512 重写 FWHT 内核进一步提升收益。

## 背景

### 问题

[`RotationMatrix::Apply`](src/rabitq/rabitq_rotation.cpp) 是搜索热路径的最大瓶颈。perf profile 在 COCO-100k (dim=512, nlist=2048, nprobe=150) 显示：

| 函数 | 占搜索时间 |
|------|----------|
| `RotationMatrix::Apply` | **42.65%** |
| `IPExRaBitQ` | 29.37% |
| `PrepareQueryInto` | 7.13% |
| `BuildFastScanLUT` | 4.69% |
| 其他 | ~16% |

`RotationMatrix::Apply` 当前实现是 O(L²) 矩阵向量乘 — 对 dim=512 是 262,144 次 FMA。每个 cluster 都要算一次（86-150 calls/query），单次查询 ~3.98 ms 都花在这里。

### 已存在但未启用的优化

代码里早已有 [`GenerateHadamard()`](src/rabitq/rabitq_rotation.cpp:144) 和 [`FWHT_InPlace()`](src/rabitq/rabitq_rotation.cpp:129)，能把 Apply 复杂度从 O(L²) 降到 O(L log L)：

- dim=512: 262,144 ops → ~5,120 ops（**51× 更少**）
- dim=128: 16,384 ops → ~896 ops（**18× 更少**）

但 `bench_vector_search.cpp` 一直只调用 `GenerateRandom()`（Gram-Schmidt QR），从未调用 `GenerateHadamard()`。所以快速路径完全没启用。

### 实测验证

启用 Hadamard 后在 COCO-100k 上跑 5 次：

| 指标 | Random (baseline) | Hadamard | Δ |
|------|------------------|----------|---|
| latency avg (median of 5) | 7.47 ms | **4.43 ms** | -41% |
| latency p99 | ~14 ms | 8.14 ms | -42% |
| recall@1 | 0.9500 | **0.9750** | +2.5% |
| recall@5 | 0.9320 | **0.9580** | +2.6% |
| recall@10 | 0.9540 | 0.9540 | 不变 |
| build time | 40 s | 31 s | -23% |

profile 验证 `RotationMatrix::Apply` 占比从 42.65% → **5.74%**，从 #1 瓶颈直接出局。

## 范围

### 本提案包含

1. **启用 Hadamard 路径**：在 benchmark 中检测 `IsPowerOf2(dim)` 时优先调用 `GenerateHadamard()`，否则回退到 `GenerateRandom()`。打印当前选择的 rotation 类型。

2. **dim=96 (deep1m) padding 方案**：把 dim=96 的向量末尾补 32 个 0，padding 到 dim=128（下一个 2^k）。所有 base/query/centroid 都做相同 padding。padding 不改变 L2 距离（补零保持点积和模长），所以理论上 recall 应该不变或更好。允许 recall 微小变化（最多 ±1%）。

3. **AVX-512 重写 FWHT_InPlace**：当前 `FWHT_InPlace` 是标量。`len >= 16` 的层（butterfly 跨度≥16）可以用 `_mm512_loadu_ps` 一次处理 16 个 lane。dim=512 的 9 层中有 5 层 (len=16,32,64,128,256) 可以完全 SIMD 化。

### 不在范围内

- 修改 RaBitQEncoder 的 rotation 接口（`RotationMatrix` 已经是抽象的，Encoder 只 call Apply）
- 重构 `data_` 矩阵存储（当 use_fast_hadamard_=true 时其实不需要存矩阵，但保留兼容性）
- IPExRaBitQ 进一步优化（已经手写 AVX-512）
- 多线程搜索

## 成功标准

1. **COCO-100k (dim=512)**: latency avg ≤ 4.5 ms（CRC=0, nprobe=150），recall@10 ≥ 0.95
2. **deep1m (dim=96 → padding 128)**: latency avg ≤ 1.43 ms（与 padding 前持平或更好），recall@10 ≥ 0.95
3. **dim=96 fallback 验证**：不开 padding 时，dim=96 仍能跑（自动 fallback 到 Random），latency 与之前一致
4. **AVX-512 FWHT**: 在 COCO-100k 上比标量 FWHT 再快 ≥ 30%（4.43 ms → ≤ 3.1 ms）
