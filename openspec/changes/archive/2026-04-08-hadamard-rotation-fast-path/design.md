# 设计文档：Hadamard 旋转快速路径

## 总体思路

```
现状:
══════════════════════════════════════════
Phase A: GenerateRandom (Gaussian QR) → 矩阵存储
Phase B: Apply = O(L²) 矩阵向量乘
         dim=512: 262144 FMA / call
         dim=96:    9216 FMA / call

目标:
══════════════════════════════════════════
Phase A: 优先 GenerateHadamard (Walsh-Hadamard × random sign)
         dim 是 2^k 时启用; 否则 fallback Random
Phase B: Apply = O(L log L) FWHT + sign + scale
         dim=512:   ~5120 ops + sign flip
         dim=128:    ~896 ops + sign flip
         (dim=96 通过 padding 到 128 启用)

加速:
  COCO  (512): 7.47ms → 4.43ms (-41%)  实测
  deep1m (96 → 128): 1.43ms → ?  待验证
```

## 设计决策

### D1: 在 benchmark 中自动选择 rotation 类型

**问题**：`GenerateHadamard` 已经存在，但 `bench_vector_search.cpp` 一直只调用 `GenerateRandom`。

**方案**：在 benchmark 创建 rotation 后，先尝试 `GenerateHadamard`，失败再退回 `GenerateRandom`：

```cpp
RotationMatrix rotation(dim);
bool used_hadamard = (dim > 0 && (dim & (dim - 1)) == 0)
                   && rotation.GenerateHadamard(seed, /*use_fast_transform=*/true);
if (!used_hadamard) {
    rotation.GenerateRandom(seed);
}
Log("  Rotation type: %s\n", used_hadamard ? "Hadamard" : "Random");
```

`use_fast_transform=true` 让 `Apply` 走 FWHT 路径而不是矩阵乘。

**为什么放在 benchmark 而不是核心库**：核心库保持中立 — `RotationMatrix` 已经支持两种生成方式，由调用方选择。Encoder/Estimator 都不关心内部用哪种。

### D2: dim=96 padding 到 128 的策略

**问题**：dim=96 不是 2^k，Hadamard 不可用。

**方案**：在 benchmark 检测 dim 不是 2^k 时，提供 `--pad-to-pow2` 选项（默认开启），把所有向量末尾补零到下一个 2^k。

```
dim=96  → padding 到 128 (补 32 个 0)
dim=200 → padding 到 256 (补 56 个 0)
```

数学上：
```
原始向量 v ∈ R^96
padded:   v' = [v, 0, 0, ..., 0] ∈ R^128

L2 距离: ||v' - u'||² = ||v - u||² + Σ(0-0)² = ||v - u||²  (不变!)
点积:    <v', u'> = <v, u> + 0 = <v, u>                    (不变!)
模长:    ||v'|| = ||v||                                    (不变!)
```

**所以 padding 是数学等价的, recall 应该完全不变**（除了量化噪声的微小差异）。

**代价**：
- code 大小增大: 96-bit → 128-bit (+33%)
- ex_code/ex_sign 增大: 96B → 128B (+33%)
- LUT 大小增大
- 但 Hadamard 加速 远大于 这些 overhead

**实现位置**：在 benchmark 的 Phase A (LoadVectors 之后)。pad 三个数据：base、query、centroids（如果是 fvecs 加载的）。

```cpp
uint32_t orig_dim = dim;
if (pad_to_pow2 && !IsPowerOf2(dim)) {
    uint32_t new_dim = NextPowerOf2(dim);
    PadVectors(base.data, N, orig_dim, new_dim);
    PadVectors(qry.data, Q, orig_dim, new_dim);
    if (centroids loaded) {
        PadVectors(centroids, nlist, orig_dim, new_dim);
    }
    dim = new_dim;
}
```

`PadVectors` 是简单的 reshape + 补零：
```cpp
void PadVectors(std::vector<float>& data, uint32_t N,
                uint32_t old_dim, uint32_t new_dim) {
    std::vector<float> padded(N * new_dim, 0.0f);
    for (uint32_t i = 0; i < N; ++i) {
        std::memcpy(padded.data() + i * new_dim,
                    data.data() + i * old_dim,
                    old_dim * sizeof(float));
    }
    data = std::move(padded);
}
```

### D3: AVX-512 重写 FWHT_InPlace

**当前实现** ([rabitq_rotation.cpp:129](src/rabitq/rabitq_rotation.cpp#L129))：

```cpp
void FWHT_InPlace(float* vec, uint32_t n) {
    for (uint32_t len = 1; len < n; len <<= 1) {
        for (uint32_t i = 0; i < n; i += len << 1) {
            for (uint32_t j = 0; j < len; ++j) {
                float u = vec[i + j];
                float v = vec[i + j + len];
                vec[i + j]       = u + v;
                vec[i + j + len] = u - v;
            }
        }
    }
}
```

**SIMD 化分析** (n=512, 9 层)：

| Level | len | butterflies | SIMD 策略 |
|-------|-----|-------------|----------|
| 0 | 1 | 256 (相邻 lane) | 标量或 vshufps |
| 1 | 2 | 256 | 标量 / 256-bit shuffle |
| 2 | 4 | 256 | 256-bit shuffle |
| 3 | 8 | 256 | 256-bit add/sub |
| 4 | 16 | 256 | **AVX-512 16-wide** ✓ |
| 5 | 32 | 256 | **AVX-512 16-wide × 2** ✓ |
| 6 | 64 | 256 | **AVX-512 16-wide × 4** ✓ |
| 7 | 128 | 256 | **AVX-512 16-wide × 8** ✓ |
| 8 | 256 | 256 | **AVX-512 16-wide × 16** ✓ |

**Level 4-8** (len ≥ 16) 完全可以 SIMD 化，每层 256 butterflies → 16 个 vector ops，完美对齐。

```cpp
// Pseudocode for level >= 16
for (uint32_t i = 0; i < n; i += len << 1) {
    for (uint32_t j = 0; j < len; j += 16) {
        __m512 u = _mm512_loadu_ps(vec + i + j);
        __m512 v = _mm512_loadu_ps(vec + i + j + len);
        _mm512_storeu_ps(vec + i + j,       _mm512_add_ps(u, v));
        _mm512_storeu_ps(vec + i + j + len, _mm512_sub_ps(u, v));
    }
}
```

**Level 0-3** (len < 16) 仍走标量：
- 在 dim=512 上是 4 层 × 256 butterflies = 1024 ops（很小，不值得复杂化）

**SIMD 化位置**：放在 `src/simd/fastscan.cpp` 或新建 `src/simd/hadamard.cpp`。按之前的约定，所有 SIMD 函数集中在 `src/simd/` 目录。

```cpp
// include/vdb/simd/hadamard.h
namespace vdb { namespace simd {
void FWHT_AVX512(float* vec, uint32_t n);  // n must be power of 2 and >= 16
}}
```

`rabitq_rotation.cpp` 的 `FWHT_InPlace` 在 dim ≥ 16 时调用 `simd::FWHT_AVX512`，否则走原来的标量实现。

**预期收益**：
- 当前 FWHT 标量: ~5120 ops, 假设 IPC=2 → ~2.5 μs / call
- AVX-512 FWHT: levels 4-8 用 16-wide → ~80 cycles / call ≈ 40 ns
- + levels 0-3 标量 ≈ 1024 ops ≈ 500 cycles ≈ 250 ns
- 合计: ~300 ns / call (vs 2500 ns 标量 FWHT, ~8× 加速)
- profile 显示 RotationMatrix 占 5.74% × 4.43 ms ≈ 254 μs / query
- 改进后 ≈ 30 μs / query
- 节省 ~220 μs / query → 4.43 ms → ~4.2 ms

实际收益可能更小（受制于内存访问），但应该至少 -10%。

## 风险分析

| 风险 | 缓解 |
|------|-----|
| Hadamard 在 RaBitQ 上 recall 不如 Gaussian | 已实测：recall 不变或微涨（COCO recall@1 +2.5%） |
| dim=96 padding 改变 d_k 分布 | padding 是数学等价，d_k 应不变；CRC 校准会自适应 |
| AVX-512 FWHT 边界处理 bug | 单元测试对比 scalar vs SIMD 输出 bit-identical |
| 重 build 所有 codes | 已经在 benchmark 中重 build，build 时间 31s，可接受 |
| `use_fast_hadamard_` 路径未测试过 | 现有代码 [rabitq_rotation.cpp:197](src/rabitq/rabitq_rotation.cpp#L197) 已存在 6 个月，我们启用并 A/B 验证 |

## 验证矩阵

| 数据集 | dim | rotation | 期望 latency | 期望 recall@10 |
|--------|-----|----------|--------------|----------------|
| COCO-100k | 512 | Hadamard | ≤ 4.5 ms | ≥ 0.95 |
| deep1m | 96 | Random (fallback) | ≤ 1.5 ms | ≥ 0.95 (与原一致) |
| deep1m | 96 → 128 padding | Hadamard | ≤ 1.4 ms (期望) | ≥ 0.95 |
| COCO-100k + AVX-512 FWHT | 512 | Hadamard | ≤ 4.0 ms | ≥ 0.95 |
