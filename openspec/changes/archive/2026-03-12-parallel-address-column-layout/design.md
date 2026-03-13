# 设计：平行地址列布局

## 概述
当前地址管道以 `AddressBlock` 为中心，它既是内存单位也是磁盘单位。每个块存储自己的 `base_offset`、`bit_width`、`record_count`、`page_size` 和打包字节载荷。`ClusterStoreReader::EnsureClusterLoaded()` 从集群结尾部分解析块元数据，然后用 `AddressColumn::DecodeBlock()` 独立解码每个块。

此变更将问题分为两个层级：

1. **集群范围的地址编码元数据**：集群中所有常规地址块共享的参数仅存储一次。
2. **列存块载荷元数据**：逐块仅存储 `base_offset`，而 `packed_size` 由集群共享参数和尾块信息推导。
3. **平行解码执行**：读取器同时重构多个常规块，然后单独处理最后的短块。

## 当前状态
### 现今
对于 `.clu` 中的每个集群块：
- RaBitQ 代码首先存储。
- 所有地址块的打包地址载荷连续存储。
- 一个小结尾部分为每个地址块存储元数据。

逐块元数据当前包括：
- `base_offset`
- `bit_width`
- `record_count`
- `page_size`
- `packed_size`

### 问题
1. `bit_width` 和 `page_size` 在每个块中重复，尽管最优路径期望这些对常规块是统一的。
2. `record_count` 仅因为最后块可能不完整而需要；应该从集群级元数据推导。
3. 解码被组织为 `for each block -> unpack -> prefix sum -> materialize`，仅在一个块内利用 SIMD，不跨块。
4. 结尾部分比需要的更大，且混合了集群共享和逐块的关切。

## 目标和约束
### 目标
- 平行解码多个常规地址块。
- 通过固定打包大小策略使块分区稳定。
- 仅存储每块所需的最少元数据。
- 在 `ClusterStoreReader::ClusterData` 中保持解码地址物化为 `std::vector<AddressEntry>`。

### 约束
- 当前存储层已在 `EnsureClusterLoaded()` 中假设集群本地惰性加载。
- `AddressColumn` 保持为地址编码/解码逻辑的规范位置。
- `.dat` 文件和 RaBitQ 代码区域保持不变。
- 实现应在实际可行处保持现有的 `Segment` 和更高级的读取 API。

## 建议的数据模型

### 1. 将地址块描述分为集群元和块列
引入集群范围的地址描述概念：
- `page_size`：由集群块共享
- `bit_width`：由集群块中所有常规地址块共享
- `block_granularity`：所有常规块的逻辑记录计数
- `num_address_blocks`：集群块中地址块的数量
- `fixed_packed_size`：所有常规块使用的打包载荷大小
- `last_packed_size`：最后一个地址块实际使用的打包载荷大小

逐块数据变为列数组：
- `base_offsets[M]`

其中 `M = num_address_blocks`。

其中前 `M-1` 个常规块的打包载荷大小恒为 `fixed_packed_size`，最后块的打包载荷大小为 `last_packed_size`。因此不再需要 `packed_sizes[M]` 列数组。

如果实现上希望进一步压缩元数据，在不单独存储 `last_packed_size` 的情况下，也可以由以下关系推导：

$$
last\_packed\_size = address\_payload\_bytes - (M - 1) \times fixed\_packed\_size
$$

但这要求读取器先可靠地得到整个地址载荷总长度。为了让尾块解析和一致性校验更直接，本设计优先推荐显式存储 `last_packed_size`。

### 2. 固定打包大小划分规则
当今 `AddressColumn::Encode()` 按记录数切块。新策略按全局固定打包大小目标切块。

编码规则：
- 选择 `page_size` 和集群共享的 `bit_width`；
- 推导适合目标打包大小的最大记录数；
- 尽可能地将地址流切为打包载荷长度等于 `fixed_packed_size` 的常规块；
- 仅允许最后块更短。

这意味着常规路径高度统一：
- 相同的 `bit_width`
- 相同的 `page_size`
- 相同的 `packed_size`
- 每块相同的逻辑记录计数

唯一例外块是最后块。

### 3. 新的逐集群块布局
在 `.clu` 的每个集群块内：

```text
[RaBitQ codes]
[Address packed payloads for block 0..M-1]
[Cluster address trailer]
```

集群地址结尾部分：
- `page_size : u32`
- `bit_width : u8`
- `block_granularity : u32`
- `fixed_packed_size : u32`
- `last_packed_size : u32`
- `num_address_blocks : u32`
- `base_offsets[M] : u64[M]`
- `trailer_size : u32`
- `block_magic : u32`

注释：
- 逐块 `record_count` 被移除。
- `address_blocks_offset` 被移除；它推导为 `block_offset + num_records * code_entry_size()`。
- 块 `j` 的记录计数从 `entry.num_records`、`block_granularity` 和 `num_address_blocks` 推导。
- 对于 `j < M - 1`，块载荷大小恒为 `fixed_packed_size`；对于 `j = M - 1`，块载荷大小为 `last_packed_size`。
- `fixed_packed_size` 和 `last_packed_size` 被显式存储，以便读取器可以直接定位尾块并校验块布局。

### 4. 整个 `.clu` 文件布局
新的 `.clu` 文件应在设计文档中明确到文件级，而不仅是 cluster block 级。完整布局如下：

```text
┌──────────────────────────────────────────────────────────┐
│ SECTION 1: Global File Header                            │
│   magic               : u32 = 0x4C4D4356 ("VCML")       │
│   version             : u32 = 3                          │
│   num_clusters        : u32                              │
│   dim                 : u32                              │
│   rabitq.bits         : u8                               │
│   rabitq.block_size   : u32                              │
│   rabitq.c_factor     : f32                              │
│   data_file_path_len  : u32                              │
│   data_file_path      : char[kMaxPathLen]                │
├──────────────────────────────────────────────────────────┤
│ SECTION 2: Cluster Lookup Table                          │
│   Entry[i]:                                               │
│     cluster_id        : u32                              │
│     num_records       : u32                              │
│     centroid          : float[dim]                       │
│     block_offset      : u64                              │
│     block_size        : u64                              │
├──────────────────────────────────────────────────────────┤
│ SECTION 3: Cluster Blocks                                │
│   ClusterBlock[0]                                         │
│   ClusterBlock[1]                                         │
│   ...                                                     │
│   ClusterBlock[num_clusters-1]                            │
└──────────────────────────────────────────────────────────┘
```

其中单个 `ClusterBlock` 的布局为：

```text
┌──────────────────────────────────────────────────────────┐
│ CLUSTER BLOCK                                             │
│   RaBitQ codes                                            │
│     uint64[nwords] × num_records                         │
│                                                          │
│   Address payload region                                  │
│     Block[0].packed : fixed_packed_size bytes            │
│     Block[1].packed : fixed_packed_size bytes            │
│     ...                                                  │
│     Block[M-2].packed : fixed_packed_size bytes          │
│     Block[M-1].packed : last_packed_size bytes           │
│                                                          │
│   Cluster address trailer                                 │
│     page_size          : u32                             │
│     bit_width          : u8                              │
│     block_granularity  : u32                             │
│     fixed_packed_size  : u32                             │
│     last_packed_size   : u32                             │
│     num_address_blocks : u32                             │
│     base_offsets[M]    : u64[M]                          │
│     trailer_size       : u32                             │
│     block_magic        : u32 = 0x424C4356 ("VCLB")      │
└──────────────────────────────────────────────────────────┘
```

这一定义明确了：
- 文件头和 lookup table 仍保持全局统一管理；
- `block_offset` / `block_size` 仍以 cluster 为单位索引；
- cluster 内部地址区是“连续 packed payload + 尾部 trailer”的结构；
- 除最后一个地址块外，其余地址块长度固定，因此无需再为每块存一份 `packed_size`。

### 5. 存储数据结构说明
为了让实现与磁盘布局一一对应，建议在设计层明确区分以下逻辑结构：

#### 5.1 全局文件头

```cpp
struct ClusterStoreFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t num_clusters;
    uint32_t dim;
    uint8_t rabitq_bits;
    uint32_t rabitq_block_size;
    float rabitq_c_factor;
    uint32_t data_file_path_len;
    char data_file_path[kMaxPathLen];
};
```

#### 5.2 查找表条目

```cpp
struct ClusterLookupEntryDisk {
    uint32_t cluster_id;
    uint32_t num_records;
    float centroid[dim];
    uint64_t block_offset;
    uint64_t block_size;
};
```

#### 5.3 集群地址布局

```cpp
struct AddressColumnLayout {
    uint32_t page_size;
    uint8_t bit_width;
    uint32_t block_granularity;
    uint32_t fixed_packed_size;
    uint32_t last_packed_size;
    uint32_t num_address_blocks;
};
```

#### 5.4 逐块地址载荷

```cpp
struct AddressBlock {
    uint64_t base_offset;
    std::vector<uint8_t> packed;
};
```

#### 5.5 编码结果

```cpp
struct EncodedAddressColumn {
    AddressColumnLayout layout;
    std::vector<AddressBlock> blocks;
};
```

上述划分表达的是：
- `AddressColumnLayout` 描述“如何解释所有地址块”；
- `AddressBlock` 描述“每个块的变动部分”；
- `EncodedAddressColumn` 是编码阶段的完整产物；
- `.clu` 文件中真正按数组写出的逐块列只有 `base_offsets[M]`，而非 `packed_sizes[M]`。

## 内存模型变化
### `AddressBlock`
当前的 `AddressBlock` 耦合编码载荷和逐块元数据。对于新路径，它应该变成逐块变化内容的紧凑表示：
- `base_offset`
- `packed`

集群共享元数据从 `AddressBlock` 移出到新的描述符，例如 `AddressColumnLayout` 或 `ClusterAddressLayout`。

可能的形状：

```cpp
struct AddressColumnLayout {
    uint32_t page_size;
    uint8_t bit_width;
    uint32_t block_granularity;
    uint32_t fixed_packed_size;
    uint32_t last_packed_size;
    uint32_t num_address_blocks;
};

struct AddressBlock {
    uint64_t base_offset;
    std::vector<uint8_t> packed;
};
```

### `ClusterStoreReader::ClusterData`
代替持久化自描述 `AddressBlock` 的向量，存储：
- 集群地址布局；
- 块基偏移；
- 已加载的打包缓冲区；
- 解码的 `AddressEntry` 缓存。

这使加载表示与磁盘格式和解码策略匹配。

## 编码流
### `AddressColumn`
添加面向集群的编码器 API。一个可能的形状：

```cpp
struct EncodedAddressColumn {
    AddressColumnLayout layout;
    std::vector<AddressBlock> blocks;
};

static EncodedAddressColumn EncodeColumn(
    const std::vector<AddressEntry>& entries,
    uint32_t fixed_packed_size,
    uint32_t page_size = kDefaultPageSize);
```

职责：
1. 将输入大小转换为页单位。
2. 从集群中最大页单位大小选择集群共享的 `bit_width`。
3. 计算 `fixed_packed_size` 和 `bit_width` 暗示的常规块容量。
4. 将条目分区为常规块和一个可选的最终短块。
5. 对每块，存储 `base_offset` 和打包载荷；同时在布局中记录 `fixed_packed_size`、`last_packed_size` 和 `num_address_blocks`。

### `ClusterStoreWriter`
`WriteAddressBlocks()` 应写入：
1. 每块的打包载荷，按顺序；
2. 包含共享布局字段和 `base_offsets[]` 的结尾部分。

查找表行为保持不变：`block_offset` 和 `block_size` 仍描述整个集群块。

## 解码流
### 读取器端解析
`ClusterStoreReader::EnsureClusterLoaded()` 应：
1. 从查找表定位集群块；
2. 从块尾读取结尾部分；
3. 重构共享地址布局；
4. 从代码区长度推导地址载荷起始；
5. 根据 `fixed_packed_size`、`last_packed_size` 和 `num_address_blocks` 切分并读取所有打包载荷。

### 多块 SIMD 解码
为常规块添加新的解码路径，概念上：

```cpp
DecodeBatchBlocks(layout, base_offsets, packed_payloads, num_regular_blocks)
DecodeSingleBlock(layout, tail_base_offset, tail_payload, tail_record_count)
```

对于常规路径：
- 将多个块分为一个批，例如每批 4 或 8 块（取决于 SIMD 助手预期）；
- 将每块的大小解包到对齐的临时缓冲区；
- 为所有块同步运行前缀和；
- 将 `AddressEntry` 输出物化到最终解码向量。

最后块单独解码，因为其记录计数可能小于 `block_granularity`，且其打包载荷大小为 `last_packed_size` 而不是 `fixed_packed_size`。

## 算法注记
### 推导常规块容量
对于给定的集群共享 `bit_width = bw` 和固定打包载荷大小 `P` 字节：

$$
records\_per\_block = \left\lfloor \frac{8P}{bw} \right\rfloor
$$

此值必须是：
- 正数；
- 受 SIMD 友好的粒度规则约束；
- 在集群中所有常规块内稳定。

如果需要精确字节对齐，编码器可能约束 `records_per_block` 使得 `records_per_block * bw` 是字节对齐的。

### 推导地址块载荷大小
对于 `M = num_address_blocks`：

$$
packed\_size(j) =
\begin{cases}
fixed\_packed\_size, & j < M - 1 \\
last\_packed\_size, & j = M - 1
\end{cases}
$$

因此读取器可以不依赖 `packed_sizes[M]` 数组来逐块定位地址载荷。

### 推导逐块记录计数
对于 `M` 块中的块索引 `j`，集群记录计数 `N` 和常规块容量 `G`：

$$
count(j) =
\begin{cases}
G, & j < M - 1 \\
N - (M - 1)G, & j = M - 1
\end{cases}
$$

当 `M > 0` 时，读取器必须验证尾部计数在 `(0, G]` 内。

## 文件格式版本控制
这不向后兼容。`.clu` 文件版本应从 `2` 升至 `3`。

读取器行为：
- 版本 `3`：解析新的集群地址结尾部分；
- 版本 `2`：拒绝且显示清晰的不支持格式错误，除非稍后添加显式兼容性路径。

## 测试策略
### 单元测试
更新并扩展 [tests/storage/address_column_test.cpp](tests/storage/address_column_test.cpp)：
- 验证集群共享 `bit_width` 选择；
- 验证固定打包大小分区；
- 验证仅最后块可能短；
- 验证多块解码往返；
- 验证尾块解码往返。

### 存储集成测试
更新 [tests/storage/cluster_store_test.cpp](tests/storage/cluster_store_test.cpp)：
- 验证结尾部分解析和布局重构；
- 验证 `EnsureClusterLoaded()` 使用多个常规块正确解码所有地址；
- 验证无效结尾部分不变量被拒绝。

更新 [tests/storage/segment_test.cpp](tests/storage/segment_test.cpp) 和 [tests/storage/data_file_test.cpp](tests/storage/data_file_test.cpp) 以保持端到端覆盖。

## 风险
1. 单个集群共享 `bit_width` 可能略微降低异构大小分布集群的压缩。
2. 打包大小策略必须谨慎定义，以便编码器和读取器推导相同的常规块容量。
3. 当块数小于 SIMD 批宽度时，平行解码批处理应保持正确。
4. 版本升级后，现有测试和任何持久化 `.clu` 文件变为陈旧。

## 此设计解决的开放问题
- **`record_count` 应逐块存储吗？** 否。它从集群级元数据推导。
- **`page_size` 和 `bit_width` 应逐块存储吗？** 否。在新格式中内容是集群范围的。
- **如何在没有 `address_blocks_offset` 的情况下找到地址载荷起始？** 从 `block_offset + num_records * code_entry_size()` 推导。
- **还需要 `packed_sizes[M]` 数组吗？** 不需要。常规块统一使用 `fixed_packed_size`，尾块使用 `last_packed_size`。
- **不规则块如何处理？** 仅最后块可能不规则；它单独解码。
