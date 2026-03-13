# 任务：多流 SIMD 地址列解码

- [x] 将 [include/vdb/storage/address_column.h](include/vdb/storage/address_column.h) 中 `AddressBlock::base_offset` 类型从 `uint64_t` 变更为 `uint32_t`，并修复所有编译影响（EncodeSingleBlock 返回值、DecodeSingleBlock 物化公式中的类型转换）。
- [x] 更新 [src/storage/address_column.cpp](src/storage/address_column.cpp) 中 `EncodeSingleBlock` 的 `base_offset` 赋值为 `uint32_t` 截断，以及 `DecodeSingleBlock` 中物化阶段的 `uint64_t` 展宽。
- [x] 更新 [include/vdb/storage/cluster_store.h](include/vdb/storage/cluster_store.h) 和 [src/storage/cluster_store.cpp](src/storage/cluster_store.cpp) 中集群地址结尾部分的序列化/反序列化，将 `base_offsets[M]` 从 `u64[M]` 改为 `u32[M]`，并将文件版本从 `3` 升至 `4`。
- [x] 新增 [include/vdb/simd/transpose.h](include/vdb/simd/transpose.h) 和 [src/simd/transpose.cpp](src/simd/transpose.cpp)，实现 `Transpose8xN`（SoA→交错）和 `TransposeNx8`（交错→SoA）函数，使用 AVX2 8×8 子块转置（scalar fallback）。
- [x] 新增 [tests/simd/transpose_test.cpp](tests/simd/transpose_test.cpp)，覆盖：K=1,2,4,8 × 各种 count 值的转置正确性；往返一致性（transpose → inverse → 比对）。
- [x] 在 [include/vdb/simd/prefix_sum.h](include/vdb/simd/prefix_sum.h) 中新增 `ExclusivePrefixSumMulti` 声明，在 [src/simd/prefix_sum.cpp](src/simd/prefix_sum.cpp) 中实现多流垂直并行前缀和（AVX2 路径 + scalar fallback）。
- [x] 扩展 [tests/simd/prefix_sum_test.cpp](tests/simd/prefix_sum_test.cpp)，新增多流前缀和测试：K=1,2,4,8 流 × 各种 count 值；与逐流 `ExclusivePrefixSum` 的一致性对比。
- [x] 在 [include/vdb/storage/address_column.h](include/vdb/storage/address_column.h) 中新增 `DecodeMultiStream` 静态方法声明和 `kMultiStreamWidth = 8` 常量。
- [x] 在 [src/storage/address_column.cpp](src/storage/address_column.cpp) 中实现 `DecodeMultiStream`：BitUnpack 所有块 → Transpose8xN → ExclusivePrefixSumMulti → TransposeNx8 + 写回 AddressEntry（uint64 展宽）。
- [x] 重写 [src/storage/address_column.cpp](src/storage/address_column.cpp) 中 `DecodeBatchBlocks` 的常规块路径，改为调用 `DecodeMultiStream`（每批 K=8 块），保留尾块的 `DecodeSingleBlock` 路径。
- [x] 扩展 [tests/storage/address_column_test.cpp](tests/storage/address_column_test.cpp)，新增：多流解码与串行解码一致性测试；num_blocks=1,3,7,8,9,16,17 覆盖批次边界；base_offset 接近 UINT32_MAX 的边界值测试。
- [x] 更新 [tests/storage/cluster_store_test.cpp](tests/storage/cluster_store_test.cpp)、[tests/storage/segment_test.cpp](tests/storage/segment_test.cpp) 和 [tests/storage/data_file_test.cpp](tests/storage/data_file_test.cpp) 以适配版本 4 格式和 uint32 base_offset。
- [x] 更新 [CMakeLists.txt](CMakeLists.txt) 以包含新的 transpose 源文件和测试文件。
- [x] 运行全部存储和 SIMD 相关测试套件，修复由变更引入的任何回归。
