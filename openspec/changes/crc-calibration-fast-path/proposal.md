## Why

Phase C（CRC 校准）在当前实现中存在严重的冗余开销，使其在 deep1m 上耗时 ~37 秒、在 coco_100k 上耗时 ~16 秒。分析根因有三个独立冗余点：

**冗余 1（最大）：`PackSignBitsForFastScan` 重复执行**

Phase B 已经对每个 cluster 的所有 32 维批次执行了 `PackSignBitsForFastScan`，将 sign bits 打包为 VPSHUFB-friendly 格式，结果存储在 `cluster_fs_blocks[cid][b].packed`。然而 Phase C 的 `ComputeScoresForQueryRaBitQ`（`crc_calibrator.cpp:145`）在每次 per-block 调用时**重新**从 raw codes 解包再打包：

```cpp
// crc_calibrator.cpp line 134–146 (Phase C inner loop, every 32-vector block)
std::vector<rabitq::RaBitQCode> temp_codes(block_count);   // heap alloc per block
for (uint32_t j = 0; j < block_count; ++j) {
    // 从 codes_block 读取 → temp_codes[j]（反序列化）
}
storage::PackSignBitsForFastScan(                           // 重新打包！
    temp_codes.data(), block_count, dim, packed_block.data());
```

而 `cluster_fs_blocks` 中已经有完全相同的 `packed_block`，只需直接传入即可。

**冗余 2：per-cluster `PrepareQuery` 含完整 FWHT**

`crc_calibrator.cpp:126` 对每个 cluster 调用 `PrepareQuery`（含 FWHT）。Phase C 探测 **全部 nlist 个 cluster**（deep1m: 4096 次，coco: 2048 次），而 Phase D 只探测 nprobe 个。冗余比 Phase D 高 4096/100 = 41× 倍。

**冗余 3：inner loop heap 分配**

`std::vector<rabitq::RaBitQCode> temp_codes(block_count)` 在每个 32-vector block 的内层循环中分配，每个 cluster ~10 次，每个 query × nlist 共数万次 heap alloc。

**当前测量基线（performance governor 稳定态）：**
- COCO 100K Phase C: ~16 秒（含数十万次 PackSignBitsForFastScan 调用）
- Deep1M Phase C: ~37 秒（nlist=4096 × 200 calibration queries × ~30 blocks/cluster）

Phase C 的目的是校准 CRC（Conformal Risk Control）的 λ 参数，使 Phase D 查询能够提前终止（coco early_stop_rate=1.00，deep1m=0.19）。Phase C 的正确性不需要改变，只需消除冗余计算。

## What Changes

- **`src/index/crc_calibrator.h`**：扩展 `ClusterData` 结构体或新增 `FsClusterData`，增加 `const uint8_t* fs_packed_block` 和 `const float* fs_norms` 字段，指向 Phase B 已打包数据
- **`src/index/crc_calibrator.cpp`**：修改 `ComputeScoresForQueryRaBitQ`，当 `cluster.fs_packed_block != nullptr` 时直接使用预打包数据，跳过 `temp_codes` 重建和 `PackSignBitsForFastScan` 调用
- **`benchmarks/bench_vector_search.cpp`**：Phase C 中构建 `ClusterData` 时，补充 `fs_packed_block`/`fs_norms` 指向 `cluster_fs_blocks[cid][b].packed.data()` / `cluster_fs_blocks[cid][b].norms.data()`

次要优化（可一并实施）：
- 将 `packed_block` 和 `temp_codes` 提升至外层 cluster 循环之外（消除 inner-loop heap alloc）
- 利用 precompute-rotated-centroids 方案（已有 proposal）消除 per-cluster FWHT

## Capabilities

### Modified Capabilities

- `CRC 校准速度`：Phase C 耗时从 O(nlist × nprobe_calib × blocks_per_cluster × pack_cost) 降至 O(nlist × nprobe_calib × blocks_per_cluster × fastscan_cost)，预期减少 60–80%
- `构建阶段总耗时`：Phase B+C 总时间显著压缩

### Unchanged Capabilities

- CRC 校准的统计正确性：lamhat、kreg、reg_lambda 数值不变
- Phase D 查询召回率与延迟：CRC 参数不变，early stopping 行为完全一致
- `--crc 0` 路径：无影响

## Impact

- **代码路径**：
  - `src/index/crc_calibrator.h` — `ClusterData` 新增 FastScan 预打包指针字段
  - `src/index/crc_calibrator.cpp` — `ComputeScoresForQueryRaBitQ` 跳过 PackSignBitsForFastScan
  - `benchmarks/bench_vector_search.cpp` — Phase C 中填充 `fs_packed_block`/`fs_norms`
- **内存**：零额外内存，直接复用 Phase B 的 `cluster_fs_blocks` 数据
- **兼容性**：非破坏性变更；`ClusterData` 新字段可为 nullable，为 null 时退回原路径；不影响 CRC 算法逻辑

## Non-Goals

- 不修改 CRC 校准的统计算法（RAPS、lamhat 求解逻辑）
- 不改变 Phase D 的查询路径
- 不将 Phase C 结果持久化到磁盘（构建时每次重算）
- 不并行化 Phase C 的校准查询
