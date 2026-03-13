# 提案：平行地址列布局

## 摘要
重构 `.clu` 集群块内部的地址列编码、磁盘布局和解码管道，使得地址元数据以列存-集群级的形式存储，且固定大小的打包地址块可以跨多个块进行 SIMD 并行解码。

## 动机
当前设计只在单个 `AddressBlock` 内优化解码。每个块冗余地存储 `page_size`、`bit_width` 和 `record_count`，集群读取器逐个解码块。这在查询热路径中浪费了性能，并使 `.clu` 块结尾部分不必要地增大。

期望的模型是：
- 在单个集群块内，仅存储一次集群范围的地址解码参数；
- 将每块的基地址以列形式存储，并通过 `fixed_packed_size` 与 `last_packed_size` 推导各块载荷大小；
- 为常规地址块选择固定的打包载荷大小，使大多数块可以同步解码；
- 跨多个块进行 SIMD 并行解码常规块，仅最后的短块单独处理。

## 变更内容
1. 将地址列抽象从逐块自描述元数据重新设计为集群范围元数据加列存的逐块数组。
2. 引入固定的 `packed_size` 目标作为主要的地址块划分规则。
3. 更改 `.clu` 逐集群块结尾部分格式以存储：
   - 集群共享的地址参数；
   - `base_offsets[]` 列数组；
   - `fixed_packed_size` 和 `last_packed_size`，用于推导各块打包载荷边界；
   - 足以从集群级元数据推导记录计数的信息（无需逐块存储）。
4. 添加多块解码路径以同时解码多个常规地址块。
5. 更新 `ClusterStoreWriter` 和 `ClusterStoreReader` 以写入/读取新布局。
6. 更新测试以验证新的磁盘布局和多块解码的正确性。

## 目标
- 减少 `.clu` 文件中冗余的逐块元数据。
- 启用跨多个地址块的 SIMD 并行解码。
- 保持确定性的地址重构。
- 支持最后的短块，但不损害常规路径性能。

## 非目标
- 改变 `.dat` 文件布局。
- 改变 RaBitQ 代码布局。
- 引入异步 I/O 或查询调度变化。
- 优化单条记录地址查询（超出已有解码缓存的范围）。

## 影响
### 受影响代码
- [include/vdb/storage/address_column.h](include/vdb/storage/address_column.h)
- [src/storage/address_column.cpp](src/storage/address_column.cpp)
- [include/vdb/storage/cluster_store.h](include/vdb/storage/cluster_store.h)
- [src/storage/cluster_store.cpp](src/storage/cluster_store.cpp)
- [tests/storage/address_column_test.cpp](tests/storage/address_column_test.cpp)
- [tests/storage/cluster_store_test.cpp](tests/storage/cluster_store_test.cpp)
- [tests/storage/segment_test.cpp](tests/storage/segment_test.cpp)
- [tests/storage/data_file_test.cpp](tests/storage/data_file_test.cpp)

### 兼容性
这是 `.clu` 文件的磁盘格式变更。新读取器应该拒绝旧格式文件或显式版本门控。文件版本应该升级。

## 成功标准
- 新的 `.clu` 文件使用集群范围的地址结尾部分。
- 新的 `.clu` 文件不再为每个地址块存储 `packed_size` 数组。
- `EnsureClusterLoaded()` 从新布局正确重构所有地址。
- 常规块通过多块 SIMD 路径解码。
- 现有存储和分段测试在更新为新格式后通过。
