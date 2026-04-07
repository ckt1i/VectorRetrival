# 设计文档：Abseil 风格的向量搜索优化

## 概览

每项优化都很小、局部、可独立测试。它们可以一项一项应用并测量。顺序的选择原则是：先做收益最大、风险最小的改动。

## 优化细节

### O1: `std::sort` -> `std::partial_sort`（Hint #5）

**当前代码** (`bench_vector_search.cpp:671`):
```cpp
std::sort(centroid_dists.begin(), centroid_dists.end());
```

对全部 4096 个元素做排序，复杂度 `O(N log N)` 约 49K 次比较。但我们只消费前 `nprobe = 100` 个。

**改为**:
```cpp
std::partial_sort(centroid_dists.begin(),
                  centroid_dists.begin() + nprobe,
                  centroid_dists.end());
```

`partial_sort` 用堆做选择：`O(N log K)` 约 27K 次比较（K=100）。更优的是 `nth_element + sort`：`O(N + K log K)` 约 4.8K 次比较，但 `partial_sort` 更简单且已经够用。

**预期节省**: ~0.1ms（centroid 排序占比从 10% 降到约 3%）。

**风险**: 无 —— 标准库函数，结果完全一致。

### O2: 合并 `soa_norms` + `soa_xipnorm`（Hint #8）

**当前**: 两个独立的 `std::vector<float>` 数组：
```cpp
std::vector<float> soa_norms(N);     // 4MB
std::vector<float> soa_xipnorm(N);   // 4MB
```

每次 Stage 2 访问都需要读 `soa_norms[vid]` 和 `soa_xipnorm[vid]`，它们在两个不同的 cache line 上（由于 `vid` 是随机的，可能产生两次 DRAM 访问）。

**改为**:
```cpp
struct alignas(8) NormPair { float norm; float xipnorm; };
std::vector<NormPair> soa_norm_pairs(N);  // 8MB, 连续
```

一次 8 字节加载就能从同一个 cache line 拿到两个值。把 norm/xipnorm 的 DRAM 往返次数减半（`ex_code`/`ex_sign` 的访问保持不变）。

**预期节省**: ~0.1-0.15ms（685K 次 Stage 2 调用 × 每次约 150ns）。

**风险**: 低 —— 纯内存布局变更。

### O3: 把 `est_kth` 提到内层循环外（Hint #22）

**当前** (`bench_vector_search.cpp:728-730`，FastScan 路径):
```cpp
for (uint32_t j = 0; j < fb.count; ++j) {
    ...
    float est_kth = (est_heap.size() >= top_k)
        ? est_heap.front().first
        : std::numeric_limits<float>::infinity();
    // est_heap.size() / front() 每个向量都会被调用
}
```

`est_kth` 只在 `est_heap` 被更新时才会变化，而 heap 被更新的概率约 0.5%（只有满足阈值的 SafeIn 或 Uncertain）。

**改为**: 用一个本地变量追踪 `est_kth`，只在 heap 修改后刷新：
```cpp
float est_kth = std::numeric_limits<float>::infinity();
auto refresh_kth = [&]() {
    est_kth = (est_heap.size() >= top_k)
        ? est_heap.front().first : INFINITY;
};

for (uint32_t j = 0; j < fb.count; ++j) {
    ...
    classify(est_dist_s1, margin_s1, est_kth);
    ...
    if (heap_was_modified) refresh_kth();
}
```

**预期节省**: ~0.05ms（每次 query 省掉约 25K 次 `est_heap.front()` 访问）。

**风险**: 低 —— 正确性依赖于"每次 heap 修改后都刷新"。在 debug 构建中加 assert 来锁定这个不变量。

### O4: 向量化 FastScan 反量化（Hint #24）

**当前** (`rabitq_estimator.cpp:248-256`):
```cpp
for (uint32_t v = 0; v < count; ++v) {
    float ip_raw = (float(raw_accu[v]) + fs_shift) * fs_width;
    float ip_est = (2.0f * ip_raw - sum_q) * inv_sqrt_dim_;
    float dist_sq = block_norms[v] * block_norms[v] + pq.norm_qc_sq
                  - 2.0f * block_norms[v] * pq.norm_qc * ip_est;
    out_dist[v] = std::max(dist_sq, 0.0f);
}
```

在向量化的 `AccumulateBlock` 之后，又用了一个标量循环逐个处理 32 个值。编译器可能会自动向量化也可能不会 —— 显式写出更可靠。

**改为**: 用 AVX-512 intrinsics 每次迭代处理 16 个 lane（count=32 时跑 2 次迭代）：
```cpp
__m512 v_shift = _mm512_set1_ps(float(pq.fs_shift));
__m512 v_width = _mm512_set1_ps(pq.fs_width);
__m512 v_sum_q = _mm512_set1_ps(pq.sum_q);
__m512 v_inv_sqrt = _mm512_set1_ps(inv_sqrt_dim_);
__m512 v_norm_qc = _mm512_set1_ps(pq.norm_qc);
__m512 v_norm_qc_sq = _mm512_set1_ps(pq.norm_qc_sq);
__m512 v_two = _mm512_set1_ps(2.0f);
__m512 v_zero = _mm512_setzero_ps();

for (uint32_t v = 0; v < count; v += 16) {
    __m512 raw = _mm512_cvtepi32_ps(_mm512_loadu_si512((__m512i*)(raw_accu + v)));
    __m512 ip_raw = _mm512_mul_ps(_mm512_add_ps(raw, v_shift), v_width);
    __m512 ip_est = _mm512_mul_ps(_mm512_fmsub_ps(v_two, ip_raw, v_sum_q), v_inv_sqrt);
    __m512 norms = _mm512_loadu_ps(block_norms + v);
    __m512 dist = _mm512_fmadd_ps(norms, norms, v_norm_qc_sq);
    dist = _mm512_fnmadd_ps(_mm512_mul_ps(v_two, _mm512_mul_ps(norms, v_norm_qc)), ip_est, dist);
    dist = _mm512_max_ps(dist, v_zero);
    _mm512_storeu_ps(out_dist + v, dist);
}
```

需要处理 `count < 32` 的标量尾部（cluster 的最后一个 block 可能不满）。

**预期节省**: ~0.05ms（FastScan dequant 占比从约 2% 降到 <1%）。

**风险**: 中等 —— intrinsics 容易出错。需要在测试数据上验证结果按位一致。

### O5: 消除 PrepareQueryInto 中冗余的 `memset`（Hint #16）

**当前** (`rabitq_estimator.cpp` PrepareQueryInto):
```cpp
pq->sign_code.resize(words_per_plane_);
std::memset(pq->sign_code.data(), 0,
            words_per_plane_ * sizeof(uint64_t));
...
pq->fastscan_lut.resize(lut_size + 63);
...
std::memset(pq->lut_aligned, 0, lut_size);  // 死代码 —— BuildFastScanLUT 会覆写
```

对于 `dim=96`，`sign_code` 只有 2 个 uint64_t = 16 字节。对这么小的 buffer 调 `memset` 有函数调用开销。`fastscan_lut` 的 memset 是冗余的 —— `BuildFastScanLUT` 会写每一个字节。

**改为**:
```cpp
// sign_code: 直接 store 清零 (避免 memset 调用开销)
pq->sign_code.resize(words_per_plane_);
for (uint32_t i = 0; i < words_per_plane_; ++i) {
    pq->sign_code[i] = 0;
}

// fastscan_lut: 跳过 memset, BuildFastScanLUT 会覆盖
pq->fastscan_lut.resize(lut_size + 63);
// (不再 memset)
```

**预期节省**: ~0.03ms（86 次 cluster 调用 × 每次约 300ns）。

**风险**: 低 —— 需要确认 `BuildFastScanLUT` 写每一个字节（确认过会的 —— 循环覆盖了全部 `lut_size` 字节）。可在 debug build 加 assert 或测试锁定。

### O6: 向量化 SafeOut 分类（Hint #1 + #15）

**背景**: 用 LTO 检查后确认 `ClassifyAdaptive` 已经被内联到 main 中，无函数调用开销。但 32 个向量仍然是逐个比较 + 分支，存在以下问题：
- 32 次循环开销 + 32 次分支预测（虽然 SafeOut 概率 84% 但仍有 ~5% 误判率）
- 每次都要做 `vcomiss + ja` 序列
- SafeOut 路径上还要做统计计数自增

**当前** (`bench_vector_search.cpp` FastScan 路径):
```cpp
for (uint32_t j = 0; j < fb.count; ++j) {
    uint32_t vid = members[local_idx];
    float est_dist_s1 = fs_dists[j];
    float margin_s1 = margin_factor * fb.norms[j];
    ResultClass rc_s1 = conann.ClassifyAdaptive(est_dist_s1, margin_s1, est_kth);
    if (rc_s1 == SafeOut) { s1_safeout++; final_safeout++; continue; }
    ...
}
```

**改为**: 用 AVX-512 一次比较 16 个 dist，得到 16-bit SafeOut mask，再用 `ctz` 循环只处理非 SafeOut 的向量。

```cpp
// 假设 dim=96, fb.count=32 → 2 次 16-lane 迭代
__m512 v_two = _mm512_set1_ps(2.0f);
__m512 v_est_kth = _mm512_set1_ps(est_kth);
__m512 v_mfactor = _mm512_set1_ps(margin_factor);

for (uint32_t base = 0; base < fb.count; base += 16) {
    __m512 v_dists   = _mm512_loadu_ps(fs_dists + base);
    __m512 v_norms   = _mm512_loadu_ps(fb.norms.data() + base);
    __m512 v_margins = _mm512_mul_ps(v_mfactor, v_norms);  // per-vector margin
    __m512 v_so_thr  = _mm512_fmadd_ps(v_two, v_margins, v_est_kth);

    // dist > est_kth + 2*margin → SafeOut
    __mmask16 so_mask = _mm512_cmp_ps_mask(v_dists, v_so_thr, _CMP_GT_OQ);

    // 统计 SafeOut 数量 (popcount)
    uint32_t so_count = __builtin_popcount(so_mask);
    s1_safeout += so_count;
    final_safeout += so_count;

    // 对非 SafeOut 的 lane 做后续处理
    uint32_t maybe_in = (~so_mask) & ((1u << std::min(16u, fb.count - base)) - 1);
    while (maybe_in) {
        int j_offset = __builtin_ctz(maybe_in);
        maybe_in &= maybe_in - 1;
        uint32_t j = base + j_offset;
        // 走原来的 SafeIn / Uncertain / Stage 2 / rerank 路径
        ...
    }
}
```

**关键观察**:
- `_mm512_cmp_ps_mask` 一条指令完成 16 个比较 → 16 个 SafeOut 决策
- `popcount` 一条指令完成 16 个 SafeOut 计数
- 84% 的向量被 mask 直接消除，连循环体都不进
- 只有 ~14% 的向量进入 ctz 循环，做完整的 SafeIn/Uncertain 判断

**预期节省**: ~0.10-0.15ms。主要来自：
- 减少 32 次循环开销 + 分支误判
- 统计计数的循环依赖被打破
- 更紧凑的 hot path → 更好的 icache 命中

**风险**: 中等
- 需要确保 mask 处理对 `count < 16` 的最后一个 block 正确
- per-vector `margins` 的计算从标量改为向量，要保证数值一致
- 单元测试应当覆盖 `count = 1, 7, 16, 17, 31, 32` 等边界

## 风险分析

| 风险 | 缓解措施 |
|------|---------|
| O4 自动向量化在编译器升级后回归 | 在测试输入上对比按位一致；加单元测试 |
| O5 中 `BuildFastScanLUT` 没覆盖所有字节 | 仔细读函数；debug 构建加 assert |
| O3 `est_kth` 失效 | 每次 heap 修改后刷新；debug 加不变量 assert |
| O1 `partial_sort` 顺序不稳定 | centroid 顺序只看距离值，并列情况罕见且无影响 |
| O6 mask 边界处理错误 | 单测覆盖 count=1/7/16/17/31/32 边界；对比 scalar 版分类计数 |
| O6 SafeIn 数量改变 (语义偏移) | 对比 final_safein/safeout/uncertain 三个统计完全一致 |

## 验证流程

每次优化后:
1. 用 `--crc 1` 跑 `bench_vector_search` 5 次，取中位数 latency
2. 验证 recall@10 不变（误差 +/- 0.001 内）
3. 验证分类计数不变（s1_safein/safeout/uncertain 等）
4. `perf stat` 确认 cache miss 率没有回归
