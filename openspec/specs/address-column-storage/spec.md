# 规格：地址列存储和平行解码

## 新增需求

### 需求：集群范围的地址结尾部分
存储在 `.clu` 中的集群块必须将地址列元数据编码为集群范围的字段加列存的逐块数组，而不是为每个地址块存储完整的元数据元组。

#### 场景：共享参数仅存储一次
给定一个具有多个地址块的集群
当写入器将集群块序列化到 `.clu`
那么结尾部分恰好存储一个 `page_size`
且结尾部分为常规地址块恰好存储一个共享 `bit_width`
且结尾部分恰好存储一个 `block_granularity`
且结尾部分恰好存储一个 `fixed_packed_size`
且结尾部分恰好存储一个 `last_packed_size`
且结尾部分存储一个 `num_address_blocks`
且结尾部分存储一个 `base_offsets[] : u32[M]` 数组，每个地址块一个 uint32_t 页索引条目
且结尾部分不存储 `packed_sizes[]` 数组
且结尾部分不存储逐块 `record_count`
且结尾部分不存储逐块 `page_size`
且结尾部分不存储逐块 `bit_width`

#### 场景：读取器根据共享打包大小推导块边界
给定一个具有 `fixed_packed_size`、`last_packed_size` 和 `num_address_blocks` 的序列化集群块
当读取器重构地址载荷区域内每个地址块的字节边界
那么它将前 `num_address_blocks - 1` 个块视为长度为 `fixed_packed_size` 的常规块
且它将最后一个块视为长度为 `last_packed_size` 的尾块
且它不依赖逐块 `packed_size` 数组来定位块边界

#### 场景：读取器推导记录计数
给定一个具有 `num_records`、`block_granularity` 和 `num_address_blocks` 的序列化集群块
当读取器重构地址块边界
那么它推导每个常规块记录计数为 `block_granularity`
且它从剩余记录推导最后块记录计数
且如果推导的最后块计数为零或超过 `block_granularity`，它拒绝集群块

### 需求：固定打包大小分区
集群内的地址条目必须被分区，以便所有非末尾地址块使用一个固定的打包载荷大小。

#### 场景：常规块使用固定打包大小
给定一个具有足够记录以产生多个常规地址块的集群
当地址列被编码
那么每个非末尾块的打包载荷大小等于 `fixed_packed_size`
且末尾块的打包载荷大小等于 `last_packed_size`
且 `last_packed_size` 小于或等于 `fixed_packed_size`

#### 场景：单块集群
给定一个其地址条目适合一个块的集群
当地址列被编码
那么集群包含恰好一个地址块
且该块的打包载荷大小等于 `last_packed_size`
且 `last_packed_size` 小于或等于 `fixed_packed_size`

### 需求：文件级布局清晰可解析
`.clu` 文件格式必须在文件级和 cluster block 级都具备明确、可推导的布局。

#### 场景：文件由三段组成
给定一个版本 `4` 的 `.clu` 文件
当读取器解析该文件
那么它先解析全局文件头
且随后解析 cluster lookup table
且最后根据 lookup table 中的 `block_offset` 和 `block_size` 解析每个 cluster block

#### 场景：cluster block 内部结构固定
给定一个 cluster block
当读取器解析该 block
那么它先读取 `num_records * code_entry_size()` 字节的 RaBitQ code 区域
且随后读取地址载荷区域
且最后从 block 尾部解析 cluster address trailer

### 需求：共享位宽选择
地址编码器必须为集群中常规地址块选择一个共享 `bit_width`。

#### 场景：集群范围最大大小决定位宽
给定一个其地址大小跨记录变化的集群
当地址列被编码
那么共享 `bit_width` 足以表示集群中的最大页单位大小
且每个常规块都以该共享 `bit_width` 编码

### 需求：多流垂直并行 SIMD 常规块解码
读取器必须通过多流垂直并行 SIMD 管线同时解码多个常规地址块，每批最多 8 个块。管线步骤为：BitUnpack → Transpose SoA→交错 → ExclusivePrefixSumMulti → TransposeNx8 → Materialize（uint32→uint64 展宽）。

#### 场景：同时解码多个常规块
给定一个具有至少两个常规地址块和一个有效结尾部分的集群
当 `EnsureClusterLoaded()` 加载集群
那么读取器为所有常规块读取打包载荷
且读取器通过多流 SIMD 管线（每批最多 K=8 块）解码常规块
且解码的 `AddressEntry` 序列与逐块 `DecodeSingleBlock` 串行解码结果完全一致

#### 场景：末尾块单独解码
给定一个其最后地址块短于 `block_granularity` 的集群
当读取器加载集群
那么读取器通过专用末尾路径解码最后块
且解码的最后块条目与原始编码地址匹配

### 需求：版本化格式强制
集群存储读取器必须通过文件版本区分新的地址列布局。

#### 场景：读取器接受新格式
给定一个以版本 `4` 写入的 `.clu` 文件
当读取器打开文件
那么它解析新的集群范围的地址结尾部分格式

#### 场景：读取器在没有兼容性路径的情况下拒绝旧格式
给定一个以版本 `3` 或更早版本写入的 `.clu` 文件
当当前读取器打开文件
那么它返回不支持格式错误，而不是静默错误解析文件
