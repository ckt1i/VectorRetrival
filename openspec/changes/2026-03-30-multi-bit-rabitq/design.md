# Design: Multi-bit RaBitQ 量化与两阶段查询

## Overview

本设计覆盖三个子系统的变更：RaBitQ 编解码、两阶段查询逻辑、Benchmark 统计增强。

---

## 1. M-bit 编码器 (RaBitQEncoder)

### 量化公式

对旋转后的归一化向量分量 `v = rotated[i]`（范围约 [-1, 1]）：

```
bin = clamp(floor((v + 1.0) / 2.0 × 2^M), 0, 2^M - 1)
```

### Bit-plane 存储布局

```
code[] 布局 (M=2, dim=128 为例):

  word 0..1:  MSB plane (= sign bits)     ← 128 bits = 2 个 uint64_t
  word 2..3:  LSB plane                    ← 128 bits = 2 个 uint64_t

总共: M × ceil(dim/64) 个 uint64_t words

通用: planes[p] 占据 code[p * num_words .. (p+1) * num_words - 1]
  p=0 为 MSB (最高位), p=M-1 为 LSB (最低位)
```

**MSB plane 等价于 1-bit sign code**：bin 的最高位 = `(v >= 0) ? 1 : 0`，因此 Stage 1 可以直接用 `code[0..num_words-1]` 做 XOR+popcount，无需额外提取。

### RaBitQCode 结构变更

```cpp
struct RaBitQCode {
    std::vector<uint64_t> code;  // M × ceil(dim/64) words, bit-plane layout
    float norm;                   // ‖o - c‖₂ (不变)
    uint32_t sum_x;               // MSB plane 的 popcount (不变，兼容 1-bit)
    uint8_t bits;                 // M = 量化位数 (新增)
};
```

### 编码器接口变更

```cpp
class RaBitQEncoder {
public:
    RaBitQEncoder(Dim dim, const RotationMatrix& rotation, uint8_t bits = 1);
    //                                                      ^^^^^^^^ 新增
    RaBitQCode Encode(const float* vec, const float* centroid = nullptr) const;
    // 返回的 code 根据 bits_ 决定 bit-plane 数量

    uint32_t num_code_words() const;  // 返回 bits_ × ceil(dim/64)
    uint8_t bits() const;             // 新增
};
```

### M=1 退化

当 `bits=1` 时，code 只有 1 个 plane，与现有行为完全一致。

---

## 2. M-bit 估算器 (RaBitQEstimator)

### PreparedQuery 变更

```cpp
struct PreparedQuery {
    // 现有字段全部保留 ...

    // 新增 (仅 bits > 1 时填充)
    uint8_t bits;                // M
    std::vector<float> lut;     // LUT[2^M], 重建值
};
```

### LUT 预计算

在 `PrepareQuery` 中，当 `bits > 1` 时预计算：

```
对于 v ∈ {0, 1, ..., 2^M - 1}:
  LUT[v] = (-1.0 + (2*v + 1.0) / 2^M) / √L
```

LUT 大小：M=2 → 4 个 float，M=4 → 16 个 float，极小。

### 两种距离估算路径

**EstimateDistance (Stage 1 — 不变)**:
```
hamming = popcount(q_sign ⊕ code[0..num_words-1])   // MSB plane
ip_est = 1 - 2·hamming/L
dist² = norm_oc² + norm_qc² - 2·norm_oc·norm_qc·ip_est
```

**EstimateDistanceMultiBit (Stage 2 — 新增)**:
```
ip_est = 0
for i in 0..dim-1:
    v = extract_mbit_value(code, i, bits, num_words)  // 从 bit-plane 提取
    ip_est += rotated_q[i] × LUT[v]

dist² = norm_oc² + norm_qc² - 2·norm_oc·norm_qc·ip_est
```

从 bit-plane 提取第 i 维的 M-bit 值：
```
v = 0
for p in 0..M-1:
    bit = (code[p * num_words + i/64] >> (i % 64)) & 1
    v |= (bit << (M - 1 - p))   // p=0 是 MSB
```

### Estimator 接口变更

```cpp
class RaBitQEstimator {
public:
    RaBitQEstimator(Dim dim, uint8_t bits = 1);
    //                       ^^^^^^^^ 新增

    PreparedQuery PrepareQuery(const float* query, const float* centroid,
                                const RotationMatrix& rotation) const;
    // 当 bits > 1 时同时计算 LUT

    // Stage 1: 不变，使用 MSB plane
    float EstimateDistance(const PreparedQuery& pq, const RaBitQCode& code) const;

    // Stage 2: 新增，使用完整 M-bit code
    float EstimateDistanceMultiBit(const PreparedQuery& pq,
                                    const RaBitQCode& code) const;
};
```

---

## 3. 两阶段查询逻辑

### ε_ip 经验公式

```
ε_ip_s1 = 采样校准 (现有逻辑，不变)
ε_ip_s2 = ε_ip_s1 / 2^(M-1)

margin_s1 = 2 · r_max · r_q · ε_ip_s1
margin_s2 = margin_s1 / 2^(M-1)
```

### 分类流程 (per vector in cluster)

```
// Stage 1
dist_est_1 = EstimateDistance(pq, code);       // MSB popcount
rc1 = conann.Classify(dist_est_1, margin_s1);

if (rc1 == SafeIn)  → 读 vec + payload, 更新 heap
if (rc1 == SafeOut) → 跳过
if (rc1 == Uncertain && bits > 1) {
    // Stage 2
    dist_est_2 = EstimateDistanceMultiBit(pq, code);
    rc2 = conann.Classify(dist_est_2, margin_s2);

    if (rc2 == SafeIn)     → 读 vec + payload, 更新 heap
    if (rc2 == SafeOut)    → 跳过
    if (rc2 == Uncertain)  → 仅读 vec, exact rerank, 确认 top-K 后再读 payload
}
if (rc1 == Uncertain && bits == 1) {
    // 无 Stage 2, 等同于 Uncertain_S2
    → 仅读 vec, exact rerank, 确认 top-K 后再读 payload
}
```

### I/O 语义总结

| 分类结果 | 读取行为 |
|---------|---------|
| SafeIn (S1 或 S2) | 读原始向量 + payload（确信在 top-K） |
| SafeOut (S1 或 S2) | 不读取（确信不在 top-K） |
| Uncertain (最终) | 仅读原始向量 → rerank → 确认后读 payload |

---

## 4. Benchmark 统计增强

### bench_rabitq_accuracy

新增参数 `--bits M`（默认 1）。当 M>1 时：
- 额外计算 Stage 2 距离误差统计
- 输出 S1/S2 各阶段分类统计 + False SafeIn/Out 比例

### bench_vector_search

新增参数 `--bits M`（默认 1）。变更：
- 查询循环集成两阶段逻辑
- 新增 False SafeIn/Out 统计（对照 GT top-K）
- 分别输出 S1/S2 统计

### 统计量定义

```
False SafeIn  = 被分类为 SafeIn 但不在 GT top-K 中
False SafeOut = 被分类为 SafeOut 但在 GT top-K 中（严重错误！）

比例:
  False SafeIn  Rate = false_safein  / total_safein
  False SafeOut Rate = false_safeout / total_safeout
```

---

## 5. 文件变更清单

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `include/vdb/rabitq/rabitq_encoder.h` | 修改 | 添加 bits 参数、调整 RaBitQCode |
| `src/rabitq/rabitq_encoder.cpp` | 修改 | M-bit 量化 + bit-plane 编码 |
| `include/vdb/rabitq/rabitq_estimator.h` | 修改 | 添加 bits、LUT、EstimateDistanceMultiBit |
| `src/rabitq/rabitq_estimator.cpp` | 修改 | LUT 预计算 + M-bit scan |
| `benchmarks/bench_rabitq_accuracy.cpp` | 修改 | 两阶段统计 + --bits 参数 |
| `benchmarks/bench_vector_search.cpp` | 修改 | 两阶段 + False SafeIn/Out + --bits |
| `tests/rabitq/rabitq_encoder_test.cpp` | 修改 | M=2,4 编码测试 |
| `tests/rabitq/rabitq_estimator_test.cpp` | 修改 | M=2,4 估算测试 |
