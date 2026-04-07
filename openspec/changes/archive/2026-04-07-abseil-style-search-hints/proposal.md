# Abseil 风格的向量搜索优化

## 概述

将 Abseil 性能优化建议（https://abseil.io/fast/hints.html）应用到 IVF + RaBitQ + ConANN 向量搜索的热路径上。目标：将单次查询延迟从 2.0ms 降低到约 1.5ms，同时保持 deep1M 数据集上的 recall@10 >= 0.95。

## 背景

在前序的 Phase 1 + Phase 2 优化（MKL centroid 扫描、SOA 代码布局、prefetch 修复、移除 gt_set）之后，搜索延迟从 5.1ms 下降到 2.0ms，recall@10 = 0.965（启用 CRC）。

Phase 2b 的 profile 显示热路径已经比较均衡，但仍有几项 Abseil 风格的微观优化可以应用。剩余的主要开销分布如下：

| 函数 | 占搜索时间比例 | 问题 |
|------|----------|-------|
| IPExRaBitQ (Stage 2) | ~36% | 两次独立的随机访问 (norms + xipnorm) |
| main 内联代码 (L2Sqr + classify + heap) | ~19% | 每个向量都重算 est_kth, 冷热代码混合 |
| RotationMatrix::Apply | ~11% | 每个 cluster (86×) 的 FWHT |
| centroid 排序 | ~10% | 对 4096 个元素做完整 `std::sort` (其实只需要 top 100) |
| FastScan + LUT 构建 | ~16% | 每个 cluster 的开销 |

## 范围

### Phase 3 优化（本提案）

6 项具体优化，对应到具体的 Abseil hint：

1. **`std::sort` -> `std::partial_sort`**（Hint #5: 算法优化）—— centroid 排序只需要 top-nprobe，不需要完整排序。

2. **合并 `soa_norms` + `soa_xipnorm`**（Hint #8: 内存布局）—— 合并成一个 `struct {float norm, xipnorm;}` 数组，使得 Stage 2 一次 cache line 加载就能拿到所需的两个值。

3. **将 `est_kth` 提到内层循环外**（Hint #22: 把不变量移出循环）—— `est_kth` 只在 heap 被修改时才会变化（约 0.5% 的向量）。每个 block 计算一次而不是每个向量都算，可以节省约 25K 次 heap 访问。

4. **向量化 FastScan 距离反量化**（Hint #24: 特化代码）—— 把 `raw_accu[32]` 转换为 `out_dist[32]` 的标量循环，可以用 2 次 AVX-512 迭代完成。

5. **消除 PrepareQueryInto 中冗余的 `memset`**（Hint #16: 避免不必要的分配）—— `fastscan_lut` 会被 `BuildFastScanLUT` 完全覆写，前面的 `memset` 是无用功。`sign_code` 只有 16 字节，把 `memset` 替换成两个标量零值赋值。

6. **向量化 SafeOut 分类**（Hint #1: Bulk APIs + Hint #15: Bit Vectors）—— 不再对 32 个向量逐个 `ClassifyAdaptive`，改用 AVX-512 比较一次得到 16-bit SafeOut mask，只对非 SafeOut 的向量（约 14%）走后续的 Stage 2 + rerank 路径。这里 LTO 已经把 `ClassifyAdaptive` 内联了，**真正的瓶颈不是函数调用而是 32 次标量分支**。

### 不在范围内

- 用 `__attribute__((noinline))` 提取冷路径（推迟 —— 需要 PGO 验证）
- Stage 1 SafeOut 快速路径（ConANN 内部已经内联，额外收益不大）
- Deferred batch rerank（大型重构，推迟到 Phase 4）
- 多线程搜索（保持单线程，项目已确定）

## 成功标准

- deep1M 数据集上 latency avg <= 1.6ms（nprobe=100, bits=4, 启用 CRC）
- recall@10 >= 0.95
- 所有优化都可以独立测量（每次只应用一个，做完 profile 再继续）
- 现有测试无回归
