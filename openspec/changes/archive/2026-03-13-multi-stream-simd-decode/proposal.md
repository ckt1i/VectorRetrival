# 提案：多流 SIMD 地址列解码

## 摘要
将地址列的解码管线从"逐块串行 SIMD"升级为"多流垂直并行 SIMD"——利用 AVX2 寄存器的 8 条 lane 同时驱动 8 个独立地址块的前缀和与物化计算，并将 `base_offset` 从 `uint64_t` 收窄为 `uint32_t`（页索引），使核心循环全程运行在 uint32 数据路径上。

## 动机
前序变更（`parallel-address-column-layout`）已将地址块元数据提升到集群范围，并为常规块选择了固定 `packed_size`。当前的 `DecodeBatchBlocks` 名义上按 K=4 分批，但内部仍然对每个块独立调用 `DecodeSingleBlock`——每次调用各自执行 BitUnpack → ExclusivePrefixSum → 标量物化。SIMD 仅在单个块内水平使用。

期望的模型是：
- SIMD 的 8 条 lane 各自运行一个独立块的前缀和流（**垂直并行**），而非在一个块内做水平 scan；
- 前缀和与物化融合为一个循环，消除额外的 pass；
- `base_offset` 收窄为 `uint32_t` 页索引（16 TB 寻址足够），使核心循环无需 32→64 展宽；
- 数据在 BitUnpack 后经过一次 SoA→交错转置，即可进入多流内核。

## 变更内容
1. 将 `AddressBlock::base_offset` 类型从 `uint64_t` 变更为 `uint32_t`。
2. 将 `.clu` 集群地址结尾部分的 `base_offsets[M]` 从 `u64[M]` 变更为 `u32[M]`，并升级文件版本至 `4`。
3. 新增 SIMD 多流前缀和原语 `ExclusivePrefixSumMulti`，接受交错布局输入，每条 lane 独立累加。
4. 新增 8×G 转置工具函数，将 BitUnpack 输出的 SoA 布局转换为交错布局。
5. 重写 `DecodeBatchBlocks` 为多流管线：BitUnpack → Transpose → 多流 PrefixSum+Materialize → Deinterleave。
6. 最终 `AddressEntry::offset`（uint64_t 字节偏移）的展宽仅在 deinterleave/写回阶段一次性完成。
7. 更新所有受影响的编码、解码、序列化和测试代码。

## 目标
- 常规块解码吞吐提升约 2×（8 流并行 vs 逐块串行）。
- 核心前缀和+物化循环全程 uint32，无展宽开销。
- 磁盘格式节省 4×M 字节（base_offsets 从 u64 缩为 u32）。
- 保持尾块解码正确性和向后拒绝旧格式。

## 非目标
- 改变 BitUnpack 原语本身的实现（仍逐块调用）。
- 改变 `.dat` 文件布局或 RaBitQ 代码区域。
- 引入 AVX-512 路径（可作为后续扩展）。
- 改变 `AddressEntry` 的公共接口（offset 仍为 uint64_t）。

## 影响
### 受影响代码
- [include/vdb/storage/address_column.h](include/vdb/storage/address_column.h) — `AddressBlock::base_offset` 类型变更，新增多流解码 API
- [src/storage/address_column.cpp](src/storage/address_column.cpp) — 多流解码管线实现
- [include/vdb/simd/prefix_sum.h](include/vdb/simd/prefix_sum.h) — 新增 `ExclusivePrefixSumMulti` 声明
- [src/simd/prefix_sum.cpp](src/simd/prefix_sum.cpp) — 多流前缀和 SIMD 实现
- [include/vdb/simd/transpose.h](include/vdb/simd/transpose.h) — 新增 SoA↔交错转置工具（新文件）
- [src/simd/transpose.cpp](src/simd/transpose.cpp) — 转置实现（新文件）
- [include/vdb/storage/cluster_store.h](include/vdb/storage/cluster_store.h) — trailer 中 base_offsets 类型变更
- [src/storage/cluster_store.cpp](src/storage/cluster_store.cpp) — 序列化/反序列化适配
- [tests/storage/address_column_test.cpp](tests/storage/address_column_test.cpp)
- [tests/storage/cluster_store_test.cpp](tests/storage/cluster_store_test.cpp)
- [tests/simd/prefix_sum_test.cpp](tests/simd/prefix_sum_test.cpp) — 新增多流前缀和测试
- [tests/simd/transpose_test.cpp](tests/simd/transpose_test.cpp) — 新增转置测试（新文件）

### 兼容性
这是 `.clu` 磁盘格式的不兼容变更（base_offsets 宽度变化）。文件版本从 `3` 升至 `4`。新读取器必须拒绝版本 ≤ 3 的文件。

## 成功标准
- 多流前缀和原语通过交错输入/输出的正确性测试。
- 8×G 转置通过对称性和往返测试。
- `DecodeBatchBlocks` 使用多流管线，解码结果与 `DecodeSingleBlock` 逐块串行结果完全一致。
- `base_offset` 全链路使用 `uint32_t`，磁盘格式中 `base_offsets[M]` 为 `u32[M]`。
- 所有现有存储和分段测试在适配后通过。
