## Context

ε 校准是 ConANN SafeIn/SafeOut 决策边界的经验校准步骤。`eps_ip`（popcount-based IP 误差 P-percentile）和 `eps_ip_fs`（FastScan 距离误差 P-percentile）作为 RaBitQ 理论 bound 的经验修正项，替代官方 RaBitQ-Library 中的 `kConstEpsilon = 1.9` 全局常数。

当前 Deep1M Phase B 实测时序：

```
ε_ip calibration     29,015 ms  ← bench_vector_search.cpp:416-449
ε_ip_fs calibration   5,831 ms  ← bench_vector_search.cpp:541-586
```

两个循环的外层结构完全相同：

```
for each cluster k in [0, nlist):          // 4096 次
    sample up to 100 member vectors as queries
    for each query q in samples:            // 100 次
        PrepareQuery(q, centroid_k, rotation)
        for each member t in cluster:        // ~250 次
            compute some error metric
            push error into thread-unsafe result vector
```

### 为什么 ε_ip 循环慢？

热路径（`bench_vector_search.cpp:432-447`）：

```cpp
for (uint32_t t = 0; t < members.size(); ++t) {
    const auto& code = codes[t_global];
    uint32_t hamming = simd::PopcountXor(pq.sign_code.data(), code.code.data(), num_words);  // SIMD ✓
    float ip_hat = 1.0f - 2.0f * hamming / dim;

    float dot = 0.0f;
    for (size_t d = 0; d < dim; ++d) {                                                       // ★ SCALAR ★
        int bit = (code.code[d / 64] >> (d % 64)) & 1;
        dot += pq.rotated[d] * (2.0f * bit - 1.0f);
    }
    float ip_accurate = dot * inv_sqrt_dim;
    ip_errors.push_back(std::abs(ip_hat - ip_accurate));
}
```

内层标量循环对 dim=128 做 128 次位提取 + fmadd，约 **400-500 ns/次**。总调用量 ~1e8 → **40-50 秒** 的 CPU 时间，部分被 memory stalls + `std::vector::push_back` 分摊，实测 29 秒。

### 为什么 ε_ip_fs 循环 "只有" 5.8 秒？

热路径（`bench_vector_search.cpp:559-583`）：

```cpp
for (uint32_t b = 0; b < cluster_fs_blocks[k].size(); ++b) {
    const auto& fb = cluster_fs_blocks[k][b];
    alignas(64) float fs_dists[32];
    estimator.EstimateDistanceFastScan(pq, fb.packed.data(), fb.norms.data(), fb.count, fs_dists);  // SIMD ✓

    for (uint32_t j = 0; j < fb.count; ++j) {
        ...
        float true_dist = simd::L2Sqr(q_vec, base + t_global * dim, dim);  // SIMD ✓
        ...
    }
}
```

内层的 `EstimateDistanceFastScan` 和 `L2Sqr` 都已 SIMD，所以这个循环相对较快。但依然是单线程 — 4096 clusters 的外层是可并行的。

## Goals / Non-Goals

**Goals:**
- ε_ip 循环从 29 s 降至 3-6 s
- ε_ip_fs 循环从 5.8 s 降至 1-2 s
- `eps_ip` / `eps_ip_fs` 数值完全等价（同样的 sort → percentile 结果）
- `lamhat` / `recall` 完全不变

**Non-Goals:**
- 不压缩 `samples_for_eps = 100`（次级调优，可独立于本 change）
- 不修改 `CalibrateDistanceThreshold`（`d_k` 校准 4.3 s 暂不处理）
- 不改变 ε 的数学定义（本 change 只是加速其计算）
- 不引入可学习参数（经验拟合语义保持不变）

## Decisions

### Decision 1: 新增 SIMD kernel `SignedDotFromBits`

**功能**：给定 float 数组 `q[dim]` 和 bit 数组 `sign_bits[dim/64]`（每 bit 表示 +1/−1），计算 `Σ q[i] × sign(i)`。

**位置**：
- `include/vdb/simd/signed_dot.h` — header
- `src/simd/signed_dot.cpp` — 实现（AVX-512 + scalar fallback）

**接口**：

```cpp
namespace vdb::simd {

/// Computes Σᵢ q[i] × s(i) where s(i) = (bit[i] ? +1.0 : -1.0),
/// with bit[i] = (sign_bits[i/64] >> (i%64)) & 1.
///
/// Used by epsilon-ip calibration to compute the "accurate" inner product
/// of a full-precision rotated query with a 1-bit sign code, avoiding
/// the scalar bit-extract + fmadd loop.
///
/// Requires: dim % 16 == 0 preferred (padded); arbitrary dim supported via
/// masked final iteration.
float SignedDotFromBits(const float* q, const uint64_t* sign_bits, uint32_t dim);

}  // namespace vdb::simd
```

### Decision 2: AVX-512 实现策略

**核心思路**：用 bit mask 直接驱动 `_mm512_mask_sub_ps` 翻转符号，零分支。

```cpp
// Pseudocode — 处理 16 float 一次
__m512 acc = _mm512_setzero_ps();
for (uint32_t d = 0; d < dim; d += 16) {
    __m512 qv = _mm512_loadu_ps(q + d);                    // 16 floats

    // 从 sign_bits 中提取 16-bit mask for this 16-float chunk
    // Chunk offset: d bits into sign_bits; need bits [d, d+16)
    __mmask16 mask = ExtractMask16(sign_bits, d);          // 见 Decision 3

    // For bits == 1 → +q[i], bits == 0 → -q[i]
    // Use blend: where mask bit=1, keep qv; else negate
    __m512 zero = _mm512_setzero_ps();
    __m512 neg_qv = _mm512_sub_ps(zero, qv);               // -qv
    __m512 signed_qv = _mm512_mask_blend_ps(mask, neg_qv, qv);

    acc = _mm512_add_ps(acc, signed_qv);
}
return _mm512_reduce_add_ps(acc);
```

**注意**：官方的 RaBitQ convention 是 `2.0f * bit - 1.0f`，即 bit=1 → +1, bit=0 → −1。上述 blend 顺序与之一致。

### Decision 3: Bit mask 提取

从 `uint64_t[]` 中抽取任意 16-bit 窗口，有两个实现选项：

**Option A（简单）**：每 16 float 一次 `__mmask16` 抽取
```cpp
static inline __mmask16 ExtractMask16(const uint64_t* bits, uint32_t bit_offset) {
    uint32_t word_idx = bit_offset >> 6;        // /64
    uint32_t bit_in_word = bit_offset & 63;     // %64
    uint64_t lo = bits[word_idx] >> bit_in_word;
    // If bit_in_word > 48, need to fetch next word for high bits
    if (bit_in_word > 48) {
        uint64_t hi = bits[word_idx + 1] << (64 - bit_in_word);
        lo |= hi;
    }
    return static_cast<__mmask16>(lo & 0xFFFF);
}
```

**Option B（要求 dim % 64 == 0）**：每 64 float 处理 4 个 16-lane chunk，直接用 uint64 移位
```cpp
for (uint32_t w = 0; w < num_words; ++w) {
    uint64_t bits = sign_bits[w];
    for (int chunk = 0; chunk < 4; ++chunk) {
        __mmask16 mask = static_cast<__mmask16>(bits & 0xFFFF);
        bits >>= 16;
        // process 16 floats
    }
}
```

**选择 Option A**：支持任意 dim（包括非 64 倍数的 padding 情形），代价是每次 mask 提取多 ~2 条指令，可忽略。

### Decision 4: Scalar fallback 与测试对齐

**Scalar 实现**（匹配原循环行为作为 reference）：

```cpp
static float SignedDotFromBitsScalar(const float* q, const uint64_t* sign_bits, uint32_t dim) {
    float dot = 0.0f;
    for (uint32_t d = 0; d < dim; ++d) {
        int bit = (sign_bits[d / 64] >> (d % 64)) & 1;
        dot += q[d] * (2.0f * bit - 1.0f);
    }
    return dot;
}
```

**测试策略**（`tests/simd/test_signed_dot.cpp`）：
- 随机 q + 随机 sign_bits，dim ∈ {64, 96, 128, 256, 512}
- `|SIMD - Scalar| / |Scalar| < 1e-5`（浮点重排导致的微小差异）
- 边界 case：全 0 bits、全 1 bits、dim 刚好不是 16 倍数

### Decision 5: ε_ip 循环替换

**原代码**（`bench_vector_search.cpp:440-445`）：
```cpp
float dot = 0.0f;
for (size_t d = 0; d < dim; ++d) {
    int bit = (code.code[d / 64] >> (d % 64)) & 1;
    dot += pq.rotated[d] * (2.0f * bit - 1.0f);
}
float ip_accurate = dot * inv_sqrt_dim;
```

**新代码**：
```cpp
float dot = simd::SignedDotFromBits(pq.rotated.data(), code.code.data(), dim);
float ip_accurate = dot * inv_sqrt_dim;
```

### Decision 6: ε_ip 循环 OMP 并行化

**外层并行 + thread-local 结果容器**：

```cpp
const int num_threads = omp_get_max_threads();
std::vector<std::vector<float>> thread_errors(num_threads);

#pragma omp parallel
{
    int tid = omp_get_thread_num();
    auto& local = thread_errors[tid];
    local.reserve(samples_for_eps * 300);  // 预留，避免 repeated realloc

    #pragma omp for schedule(dynamic, 16)
    for (uint32_t k = 0; k < nlist; ++k) {
        // ... 原循环体，push_back 到 local 而非 ip_errors ...
    }
}

// 合并
std::vector<float> ip_errors;
size_t total = 0;
for (auto& v : thread_errors) total += v.size();
ip_errors.reserve(total);
for (auto& v : thread_errors) ip_errors.insert(ip_errors.end(), v.begin(), v.end());
```

**关键点**：
- `schedule(dynamic, 16)` — cluster 大小不均，动态调度更好
- Thread-local vector 避免 critical section / atomic
- 合并后 `sort + percentile` 与原顺序无关（P-percentile 不依赖插入顺序）
- `RaBitQEstimator::PrepareQuery` 调用需确认线程安全性 → 见 Decision 8

### Decision 7: ε_ip_fs 循环 OMP 并行化

结构完全类比 Decision 6，差异：
- 使用 `fs_normalized_errors` 作为结果名
- `std::mt19937 rng(seed + k + nlist)` 的种子计算保持不变，确保每个 cluster 的采样 deterministic

### Decision 8: 线程安全性审查

**需要验证以下对象在并发访问下安全：**

1. **`estimator.PrepareQuery`** — 读 rotation matrix，写返回的 `PreparedQuery` 结构。若 estimator 内部有 mutable cache 或共享 scratch buffer，不安全。
   - **审查项**：阅读 `src/rabitq/rabitq_estimator.cpp` 的 `PrepareQuery` 实现
   - **预期**：应为 const 方法，无共享状态。如有 scratch buffer，改为 per-call stack 分配
2. **`simd::PopcountXor`** — 纯函数，无状态，安全
3. **`simd::SignedDotFromBits`** — 纯函数，无状态，安全
4. **`simd::L2Sqr`** — 纯函数，安全
5. **`estimator.EstimateDistanceFastScan`** — 读 `PreparedQuery`，写 `fs_dists` stack buffer，安全
6. **`codes[]` / `cluster_members[]` / `centroids[]` / `base.data`** — 只读，安全
7. **`cluster_fs_blocks[k]`** — 只读，安全
8. **`std::mt19937 rng(seed + k)`** — per-iteration 局部变量，安全

**Action**：tasks.md 中的 task 2.1 显式要求审查 `PrepareQuery`，若发现共享状态则先修复（可能变为另一个小 change 的前置依赖）。

### Decision 9: CMake 集成

- `src/simd/CMakeLists.txt`（若存在）添加 `signed_dot.cpp`；否则添加到现有 simd 库目标
- 单元测试加入 `tests/CMakeLists.txt`
- OpenMP 支持：确认 `find_package(OpenMP)` 已在 root CMakeLists.txt 中启用；`target_link_libraries(bench_vector_search PRIVATE OpenMP::OpenMP_CXX)`

## Risks / Trade-offs

- **[浮点数值差异]** SIMD 重排加法顺序可能与 scalar 有 < 1e-5 相对误差。`eps_ip` 是 P-percentile 统计量，对单点误差不敏感。验证：P50 和 P95 的 eps_ip 值相对基线偏差 < 0.1%。
- **[OMP 非 deterministic 顺序]** `ip_errors` 的元素顺序取决于线程调度，但 sort + percentile 对顺序无关，最终 `eps_ip` 值完全一致（相同 float 集合的 P-percentile 唯一）。
- **[PrepareQuery 线程安全]** 未经审查前为未知风险。若 `PrepareQuery` 存在共享 mutable 状态，本 change 会引入数据竞争。tasks.md 2.1 必须先审查。
- **[性能上限]** OMP 理想情况下 8 核 ~8× 加速，但受限于：
  - 外层 cluster 大小不均（长尾 cluster 拖累 schedule）
  - NUMA/cache 干扰
  - `push_back` 的 std::allocator 底层 mutex（Linux glibc malloc 通常无 global lock，但高并发时可能成为热点）
  - 实际预期 4-6× 加速
- **[ABI/API 不变]** 新增 `simd::SignedDotFromBits` 为新符号；不修改现有签名，零破坏性变更。

## 预期收益

| 循环                | 基线       | SIMD 后    | SIMD+OMP(8×) | 预估改善 |
|--------------------|-----------|-----------|--------------|--------|
| ε_ip calibration   | 29,015 ms | ~8,000 ms | ~1,500 ms    | ~19×   |
| ε_ip_fs calibration| 5,831 ms  | ~5,800 ms | ~1,200 ms    | ~5×    |
| 两者合计             | 34,846 ms | ~13,800 ms| ~2,700 ms    | ~13×   |

**Phase B 总耗时联动：**
- 本 change 单独：90.5 s → ~58 s（-32 s）
- 本 change + `encoder-mbit-fast-quantize`：90.5 s → ~12-18 s（-72~78 s）

## 开放问题

- **samples_for_eps 是否降至 30-50**？留作 Non-Goal；若本 change 后 Phase B 仍需进一步压缩，可作为次级 change
- **d_k calibration (4.3 s) 是否并行化**？`CalibrateDistanceThreshold` 在 `ConANN` 中，结构更复杂，留作后续 change
- **SignedDotFromBits 是否值得移入 utility header 以便其他路径复用**？目前只有 ε_ip 一个用户；待有第二个需求再抽象
