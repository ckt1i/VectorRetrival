# 任务：平行地址列布局

- [x] 重新设计 [include/vdb/storage/address_column.h](include/vdb/storage/address_column.h) 中的地址列数据模型，以将集群范围布局元数据从逐块载荷数据分离。
- [x] 在 [src/storage/address_column.cpp](src/storage/address_column.cpp) 中实现新编码器，以便选择集群共享 `bit_width`，按固定 `packed_size` 分区，并显式生成 `fixed_packed_size`、`last_packed_size` 和仅一个可能短的最终块。
- [x] 在 [include/vdb/storage/address_column.h](include/vdb/storage/address_column.h) 和 [src/storage/address_column.cpp](src/storage/address_column.cpp) 中添加多块解码 API，用于常规块的 SIMD 并行解码，加上末尾块解码路径。
- [x] 升级 `.clu` 格式版本，并更新 [include/vdb/storage/cluster_store.h](include/vdb/storage/cluster_store.h) 和 [src/storage/cluster_store.cpp](src/storage/cluster_store.cpp) 以写入和解析新的集群范围地址结尾部分，以及文件级 `.clu` 布局约束。
- [x] 重构 [include/vdb/storage/cluster_store.h](include/vdb/storage/cluster_store.h) 和 [src/storage/cluster_store.cpp](src/storage/cluster_store.cpp) 中的 `ClusterStoreReader::ClusterData` 和 `EnsureClusterLoaded()`，以重构载荷起始、基于 `fixed_packed_size`/`last_packed_size` 切分载荷、批解码常规块和物化解码地址。
- [x] 为新的磁盘布局和版本控制行为，更新 [tests/storage/cluster_store_test.cpp](tests/storage/cluster_store_test.cpp)、[tests/storage/segment_test.cpp](tests/storage/segment_test.cpp) 和 [tests/storage/data_file_test.cpp](tests/storage/data_file_test.cpp) 中的存储集成测试。
- [x] 扩展 [tests/storage/address_column_test.cpp](tests/storage/address_column_test.cpp)，以覆盖固定打包大小分区、共享 `bit_width`、`last_packed_size` 语义、批常规块解码和末尾块解码。
- [x] 运行存储相关测试套件，并修复由布局变化引入的任何回归。
