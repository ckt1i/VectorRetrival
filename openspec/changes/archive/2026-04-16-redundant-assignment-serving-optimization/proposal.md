## Why

当前 `redundant-assignment-serving-optimization` 的第一轮实现已经验证了一件事：在 `data.dat` 只写一次、duplicated postings 跨多个 clusters 共享同一物理地址的设计下，现有 `.clu` 地址编码格式无法正确表示一般的 redundant assignment。

问题不只是“地址未排序”，而是现有 `AddressColumn` 的编码语义本身要求 cluster 内地址可由：

- 第一条记录的 `base_offset`
- 后续记录的 `size` 前缀和

恢复出来。也就是说，它只能正确表示**连续布局**的地址序列。对于 `redundant_top2`，secondary cluster 往往引用的是全局 `data.dat` 中带 gap 的地址，因此即使排序后也仍然无法被当前格式正确重建。

与此同时，当前查询路径已经从“按需逐块读取 `.clu` 并解压地址块”转向“统一 preload `.clu` 到内存”。这使得原先以压缩和解压为中心的 `AddressColumn` 价值明显下降。更直接的做法是把 `.clu` 的地址区改成**原始地址表**：

- 直接存每条 posting 的 `(offset_pages, size_pages)`
- 不再依赖连续布局
- preload 后直接按 view 访问
- 同时支持 single-assignment 和 redundant assignment

因此，这次 change 需要进一步升级为一次 `.clu` 格式变更：

1. 将现有压缩地址列替换为 `AddressColumn V2`
2. 将 `.clu` 文件版本升级到新的 address-table 格式
3. 在新格式上重新评估 `2048/1024 × single/redundant_top2`

## What Changes

- 将 `.clu` 的地址区从“`base_offset + packed_sizes` 的压缩地址列”升级为“每条 posting 一条原始地址项”的 `AddressColumn V2`。
- 新地址项以 `4KB` page unit 存储，建议格式为：
  - `offset_pages: uint32`
  - `size_pages: uint32`
- cluster block 的 address payload 改为一段连续的原始地址表，不再分 address blocks，不再执行地址解压。
- cluster mini-trailer 改为 V2 结构，只记录：
  - address format version
  - page size
  - address entry size
  - num address entries
  - address payload offset / bytes
  - trailer footer
- `.clu` 文件版本升级，并要求 reader 同时支持：
  - 旧格式（兼容已有 single-assignment 索引）
  - 新格式（redundant assignment 主路径）
- `ParsedCluster` / resident preload 迁移到基于 raw address table 的 view 访问；`decoded_addresses` 从热路径主结构降为兼容/测试辅助。
- benchmark 与验证流程升级为：
  - 先验证新格式下 single/redundant 的 correctness
  - 再完成四条 warm serving 对照线：
    - `2048 single`
    - `2048 redundant_top2`
    - `1024 single`
    - `1024 redundant_top2`

## Capabilities

### New Capabilities
- `redundant-assignment-serving`: 支持基于共享 `.data` 记录和原始地址表 `.clu` 编码的 top-2 redundant assignment。

### Modified Capabilities
- `e2e-benchmark`: 基于新 `.clu` 地址格式重新执行 warm serving 对照，并输出 duplication / dedup / preload 成本。

## Impact

- Affected code:
  - `include/vdb/storage/address_column.h`
  - `src/storage/address_column.cpp`
  - `include/vdb/storage/cluster_store.h`
  - `src/storage/cluster_store.cpp`
  - `include/vdb/query/parsed_cluster.h`
  - `src/query/overlap_scheduler.cpp`
  - `src/index/ivf_builder.cpp`
  - `include/vdb/index/ivf_builder.h`
  - `benchmarks/bench_e2e.cpp`
  - `tests/storage/address_column_test.cpp`
  - `tests/storage/cluster_store_test.cpp`
  - `tests/index/ivf_builder_test.cpp`
  - `tests/query/overlap_scheduler_test.cpp`
- Affected artifacts:
  - `.clu` file version and mini-trailer layout
  - warm benchmark CSV / analysis 文档
- Affected behavior:
  - 新索引将使用新的 `.clu` 地址格式
  - preload 路径不再需要地址解压
  - redundant assignment 将不再受“地址连续布局”约束
