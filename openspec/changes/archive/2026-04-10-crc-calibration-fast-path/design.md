## Context

Phase C 调用路径：

```
bench_vector_search.cpp Phase C
  └─ CrcCalibrator::CalibrateWithRaBitQ(...)
       └─ ComputeAllScoresRaBitQ(...)
            └─ ComputeScoresForQueryRaBitQ(query, ..., clusters, rotation)
                 ├─ [per cluster] PrepareQuery(query, centroid, rotation)   // FWHT
                 └─ [per block]   PackSignBitsForFastScan(temp_codes, ...)  // 重复打包！
                                  EstimateDistanceFastScan(pq, packed, ...) // 实际有用功
```

Phase B 已完成的工作（在 `cluster_fs_blocks`）：

```
cluster_fs_blocks[cid][b].packed  ← 已打包的 FastScan 布局 sign bits
cluster_fs_blocks[cid][b].norms   ← 已对齐的 float32 norms[32]
cluster_fs_blocks[cid][b].count   ← 本 block 实际向量数
```

Phase C 只需直接使用这些，跳过 `temp_codes` 反序列化和 `PackSignBitsForFastScan`。

当前耗时估算（deep1m, 200 calib queries）：
```
nlist=4096 clusters × 200 queries × ~30 blocks/cluster
= ~24.5M PackSignBitsForFastScan 调用（每次 dim=128 的 scalar bit 操作，~100 ns）
= ~2.5 秒 仅用于重复打包（在 37 秒总 Phase C 时间中）

PrepareQuery（FWHT）：4096 × 200 × ~450 ns = ~369 ms
（但 FWHT 在 Phase C 中占比相对较小，因 PackSignBits 和 L2Sqr 更贵）
```

## Goals / Non-Goals

**Goals:**
- 消除 `ComputeScoresForQueryRaBitQ` 中的 `PackSignBitsForFastScan` 重复调用
- 消除内层循环的 `std::vector<RaBitQCode>` heap 分配
- 保持 CRC 校准数值完全不变（lamhat/kreg/d_min/d_max 不变）
- Phase C 耗时预期减少 60–80%（主要来自消除 PackSignBitsForFastScan）

**Non-Goals:**
- 不修改 RAPS 统计算法
- 不并行化校准
- 不持久化校准结果

## Decisions

### Decision 1: ClusterData 接口扩展方式

`ClusterData`（在 `crc_calibrator.h`）当前只有 raw `codes_block` 指针。需要传入预打包 FastScan 数据。

**方案 A（推荐）**: 在 `ClusterData` 中新增可选字段：
```cpp
struct ClusterData {
    // existing fields ...
    const uint8_t* codes_block;       // raw codes（per-vector layout）
    uint32_t code_entry_size;
    
    // NEW: pre-packed FastScan blocks (nullable; if null, fall back to codes_block path)
    struct FsBlock {
        const uint8_t* packed;  // PackSignBitsForFastScan output
        const float* norms;     // aligned float32 norms[32]
        uint32_t count;
    };
    const FsBlock* fs_blocks;         // array of FsBlock, length = num_fs_blocks
    uint32_t num_fs_blocks;           // 0 if not available
};
```

`ComputeScoresForQueryRaBitQ` 开头检查 `cluster.num_fs_blocks > 0`，若是则走预打包路径，否则走原路径。

**方案 B**: 新增独立函数 `ComputeScoresForQueryRaBitQFast(... FsBlockData ...)`。

**选择方案 A**：ClusterData 字段可 nullable，无需 API 分裂；原路径（`num_fs_blocks=0`）保持不变，方便测试对比正确性。

### Decision 2: 内层循环 heap 分配消除

`packed_block` 和 `temp_codes` 当前在 cluster 循环内分配。

优化：在 cluster 循环**外部**（per-query 级别）预分配，复用于所有 cluster：

```cpp
// 在 per-query 开始处，cluster 循环外：
std::vector<uint8_t> packed_block(packed_sz, 0);
std::vector<rabitq::RaBitQCode> temp_codes(32);  // max block size = 32

// cluster 循环中 block 循环：直接用 packed_block.data() / temp_codes.data()
// 无需 resize（32 已够）
```

节省：nlist × n_blocks_per_cluster 次 malloc/free，约 4096 × 30 = ~120K 次/query。

### Decision 3: bench_vector_search.cpp 中 ClusterData 填充

Phase C 中构建 `clusters[cid]` 时，从 `cluster_fs_blocks` 中提取 FsBlock 指针：

```cpp
// Phase C 中构建 ClusterData（bench_vector_search.cpp）
std::vector<std::vector<ClusterData::FsBlock>> cluster_fs_meta(nlist);
for (uint32_t cid = 0; cid < nlist; ++cid) {
    const auto& fbs = cluster_fs_blocks[cid];
    cluster_fs_meta[cid].resize(fbs.size());
    for (size_t b = 0; b < fbs.size(); ++b) {
        cluster_fs_meta[cid][b] = {
            fbs[b].packed.data(),
            fbs[b].norms.data(),
            fbs[b].count
        };
    }
    clusters[cid].fs_blocks = cluster_fs_meta[cid].data();
    clusters[cid].num_fs_blocks = static_cast<uint32_t>(cluster_fs_meta[cid].size());
}
```

`cluster_fs_blocks` 在 Phase B 中构建，生命期覆盖整个 Phase C，无悬挂指针风险。

### Decision 4: ComputeScoresForQueryRaBitQ 修改

快速路径（`num_fs_blocks > 0`）实现：

```cpp
for (uint32_t p = 0; p < nlist; ++p) {
    uint32_t cid = centroid_dists[p].second;
    const auto& cluster = clusters[cid];

    auto pq = estimator.PrepareQuery(
        query, centroids + (size_t)cid * dim, rotation);

    if (cluster.num_fs_blocks > 0) {
        // FAST PATH: use pre-packed blocks directly
        for (uint32_t b = 0; b < cluster.num_fs_blocks; ++b) {
            const auto& fsb = cluster.fs_blocks[b];
            alignas(64) float fs_dists[32];
            estimator.EstimateDistanceFastScan(
                pq, fsb.packed, fsb.norms, fsb.count, fs_dists);
            // ... heap update ...
        }
    } else {
        // FALLBACK: original temp_codes + PackSignBitsForFastScan path
        // (unchanged)
    }
}
```

### Decision 5: PrepareQuery FWHT 优化（与 precompute-rotated-centroids 联动）

Phase C 中 PrepareQuery 的 per-cluster FWHT 是次要开销（~10% of Phase C time）。

本 change **不**在 Phase C 中实现 precomputed rotated centroids，原因：
1. `precompute-rotated-centroids` 提案已独立存在，应单独推进
2. Phase C 的 CRC 正确性测试应与 Phase D 的 FWHT 优化解耦
3. PackSignBitsForFastScan 的消除已可带来 60–80% 的改善，风险低

待 `precompute-rotated-centroids` 实施后，Phase C 也可联动使用 rotated centroids（在 Decision 4 的 FAST PATH 中将 `PrepareQuery` 替换为 `PrepareQueryRotatedInto`）。

## Risks / Trade-offs

- **[数值一致性]** 快速路径使用与 Phase B 完全相同的 packed 数据，FastScan 输出与原路径逐 bit 相同，lamhat 等校准结果不变
- **[ClusterData 结构膨胀]** 新增 4 个字段（2 个指针 + 2 个 uint32），但 ClusterData 本身只在 Phase C 构建时存在，内存开销可忽略
- **[向后兼容]** `num_fs_blocks=0` 时退回原路径，现有测试和 `--crc 0` 路径不受影响
- **[生命期]** `fs_blocks` 指针指向 `cluster_fs_blocks`（栈上 vector），Phase C 调用期间生命期安全；需确保 Phase C 调用在 `cluster_fs_blocks` 析构前完成（当前代码已是此顺序）

## 预期收益

| 数据集     | 当前 Phase C 耗时 | 预期优化后 | 主要来源 |
|-----------|-----------------|----------|--------|
| COCO 100K | ~16 秒          | ~3–6 秒  | 消除 PackSignBitsForFastScan |
| Deep1M    | ~37 秒          | ~5–10 秒 | 消除 PackSignBitsForFastScan + heap alloc |

（Phase C 的剩余时间主要由 PrepareQuery FWHT + L2Sqr centroid sort 决定）
