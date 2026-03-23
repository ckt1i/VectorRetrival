# Design: Norm Storage in Cluster Block + Zero-Copy ProbeCluster

## Architecture

```
  .clu 文件格式 v5 — 单条 Code Entry 布局
  ═══════════════════════════════════════════════════════════

  byte offset:  0                  num_words×8     +4      +8
                │                      │            │       │
                ▼                      ▼            ▼       ▼
               ┌────────────────────┬──────────┬──────────┐
               │  code_bits         │ norm_oc  │  sum_x   │
               │  uint64[num_words] │  float32 │  uint32  │
               │  (96 B @dim=512)   │  (4 B)   │  (4 B)   │
               └────────────────────┴──────────┴──────────┘
               │◄─────── code_entry_size = num_words×8 + 8 ────────►│

  dim=512: num_words=8, code_entry_size = 64 + 8 = 72 bytes
  dim=768: num_words=12, code_entry_size = 96 + 8 = 104 bytes
  dim=128: num_words=2, code_entry_size = 16 + 8 = 24 bytes

  对齐保证: code_entry_size ≡ 0 (mod 8) → 每条 entry 起始地址 8B 对齐 ✓
```

## 对齐与 Cache 分析

```
  Cache Line 视角 (64 bytes, dim=512, code=64B):
  ═══════════════════════════════════════════════

  Entry 0:
  ┌─── cache line 0 (64B) ──┐┌─── cache line 1 (8B used) ──┐
  │  code bits (64 bytes)    ││ norm(4B) │ sum_x(4B) │ ...  │
  └──────────────────────────┘└──────────────────────────────┘
                                    │
                 PopcountXor 读完 code 后，norm 就在下一个 cache line
                 但这个 cache line 在顺序扫描时会被硬件预取 → 几乎免费

  对比 SoA: norm 在 N×64B 之后 → N>170 时超出 L1 cache → 必定 miss
```

## 零拷贝 ProbeCluster

### Before (当前)

```cpp
// 每个向量: 1× heap alloc + memcpy + PopcountTotal
batch_codes[i].code.assign(words, words + num_words_);  // alloc + copy
batch_codes[i].norm = 0.0f;                              // BUG: norm 丢失!
batch_codes[i].sum_x = simd::PopcountTotal(words, num_words_);
```

### After (零拷贝)

```cpp
// 每个向量: 0 alloc, 直接指针操作
const uint8_t* entry = pc.codes_start + (offset + i) * pc.code_entry_size;
const uint64_t* code_ptr = reinterpret_cast<const uint64_t*>(entry);
float norm_oc = *reinterpret_cast<const float*>(entry + num_words_ * 8);
uint32_t sum_x = *reinterpret_cast<const uint32_t*>(entry + num_words_ * 8 + 4);

float dist = estimator.EstimateDistanceRaw(pq, code_ptr, num_words_, norm_oc, sum_x);
```

### 新增 RaBitQEstimator 接口

```cpp
/// 零拷贝距离估计 — 不需要构造 RaBitQCode
float EstimateDistanceRaw(
    const PreparedQuery& pq,
    const uint64_t* code_words,
    uint32_t num_words,
    float norm_oc,
    uint32_t sum_x) const;
```

实现逻辑与现有 `EstimateDistance` 完全一致，只是参数从 `RaBitQCode&` 变为原始指针。

## 文件格式变更

### ClusterStoreWriter::WriteVectors

```
Before (v4):
  for code in codes:
    write(code.code)           ← 只写 binary code

After (v5):
  for code in codes:
    write(code.code)           ← binary code
    write(code.norm)           ← float32 norm
    write(code.sum_x)          ← uint32 sum_x
```

### ClusterStoreReader::ParseBlock

```
Before: code_entry_size = num_words × 8
After:  code_entry_size = num_words × 8 + 8

// ParsedCluster 不需要改——codes_start 仍然指向 block_buf 开头
// code_entry_size 已经包含了 norm + sum_x 的空间
```

### Global Header

`version` 字段从 4 升到 5。Reader 需检查 version：
- v4: code_entry_size = num_words × 8 (旧格式，无 norm)
- v5: code_entry_size = num_words × 8 + 8 (新格式，含 norm + sum_x)

## RaBitQ 精度 Benchmark

```
  bench_rabitq_accuracy.cpp
  ═════════════════════════

  Phase 1: Load coco_1k
    image_embeddings.npy → float[1000][512]

  Phase 2: Build (nlist=32, in-memory)
    IvfBuilder → .clu + .dat

  Phase 3: RaBitQ 暴力搜索 vs 原始 L2 暴力搜索
    for each query q (1000 queries):
      for each image v (1000 images):
        exact_dist[v] = L2Sqr(q, v)                   ← ground truth
        rabitq_dist[v] = EstimateDistance(pq, code_v)  ← RaBitQ 估计

      // 距离偏差统计
      error[v] = |rabitq_dist[v] - exact_dist[v]|
      rel_error[v] = error[v] / exact_dist[v]

      // 排序对比
      exact_topk = argsort(exact_dist)[:K]
      rabitq_topk = argsort(rabitq_dist)[:K]
      recall@K = |exact_topk ∩ rabitq_topk| / K

  Phase 4: 输出统计
    ┌──────────────────────────────────────────────────┐
    │  RaBitQ Accuracy Report                          │
    ├──────────────────────────────────────────────────┤
    │  Distance Error:                                 │
    │    mean |rabitq - exact|       = ???              │
    │    max  |rabitq - exact|       = ???              │
    │    mean relative error         = ???              │
    │    p95 relative error          = ???              │
    │    p99 relative error          = ???              │
    │                                                  │
    │  Ranking Accuracy:                               │
    │    recall@1                    = ???              │
    │    recall@5                    = ???              │
    │    recall@10                   = ???              │
    │                                                  │
    │  SafeIn/SafeOut Classification:                  │
    │    SafeIn correct rate         = ???              │
    │    SafeOut correct rate        = ???              │
    │    Uncertain %                 = ???              │
    │    False SafeIn rate           = ???  (严重!)     │
    │    False SafeOut rate          = ???  (严重!)     │
    └──────────────────────────────────────────────────┘
```

### 关键度量: False SafeIn / False SafeOut

```
  False SafeIn:  RaBitQ 说 "definitely top-K" 但实际不在 top-K
    → 浪费 I/O (读了 payload 但最终不用)
    → 影响: 性能，不影响正确性

  False SafeOut: RaBitQ 说 "definitely NOT top-K" 但实际在 top-K
    → 丢失正确结果!
    → 影响: 正确性!!! 这是最危险的错误

  如果 False SafeOut 率过高 → epsilon (c_factor) 设太小
  如果 Uncertain 率过高    → epsilon 设太大，SafeIn/SafeOut 没有区分力
```

## Changes Summary

| 文件 | 变更 |
|------|------|
| `include/vdb/common/types.h` | 无变化 (RaBitQCode 结构保持) |
| `include/vdb/storage/cluster_store.h` | version 常量 4→5 |
| `src/storage/cluster_store.cpp` | WriteVectors 写 norm+sum_x; ParseBlock 用新 entry size |
| `include/vdb/rabitq/rabitq_estimator.h` | 新增 `EstimateDistanceRaw()` |
| `src/rabitq/rabitq_estimator.cpp` | 实现 `EstimateDistanceRaw()` |
| `src/query/overlap_scheduler.cpp` | ProbeCluster 零拷贝重写 |
| `tests/index/bench_rabitq_accuracy.cpp` | 新文件: RaBitQ 精度 benchmark |
| `CMakeLists.txt` | 添加 bench target |
