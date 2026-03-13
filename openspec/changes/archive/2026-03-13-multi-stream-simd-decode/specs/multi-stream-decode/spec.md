# 规格：多流 SIMD 地址列解码

## 新增需求

### 需求：base_offset 类型收窄
`AddressBlock::base_offset` 必须从 `uint64_t` 变更为 `uint32_t`，表示页索引（page index）。

#### 场景：uint32_t 页索引编码
给定一组 AddressEntry，其中最大字节偏移为 `max_offset`，page_size 为 `ps`
当地址列被编码
那么每个块的 `base_offset` 类型为 `uint32_t`
且 `base_offset = first_record_byte_offset / ps`
且 `base_offset` 不超过 `UINT32_MAX`

#### 场景：字节偏移从 uint32 页索引正确重构
给定一个 `base_offset`（uint32_t）和 `prefix[i]`（uint32_t）以及 `page_size`
当解码器重构 AddressEntry
那么 `entry.offset = static_cast<uint64_t>(base_offset + prefix[i]) * page_size`
且 `entry.offset` 为 `uint64_t` 类型

#### 场景：磁盘格式使用 u32 base_offsets
给定一个版本 `4` 的 `.clu` 文件
当写入器序列化集群地址结尾部分
那么 `base_offsets[M]` 数组中每个元素为 4 字节（u32）
且结尾部分总大小比版本 3 减少 `4 × M` 字节

### 需求：交错缓冲区布局
多流解码管线必须使用交错（interleaved）内存布局，使同一元素索引下所有流的值连续存放。

#### 场景：SoA 到交错转置
给定 K 个长度为 G 的 uint32_t 数组（SoA 布局）
当执行 `Transpose8xN` 转置
那么输出缓冲区大小为 `G × 8` 个 uint32_t
且 `output[j * 8 + k] == input_streams[k][j]` 对所有有效的 j, k 成立
且当 `k >= K` 时 `output[j * 8 + k] == 0`

#### 场景：交错到 SoA 反转置
给定一个 `G × 8` 交错缓冲区
当执行 `TransposeNx8` 反转置
那么 `output_streams[k][j] == input[j * 8 + k]` 对所有有效的 j, k 成立

#### 场景：转置往返一致性
给定 K 个长度为 G 的随机 uint32_t 数组
当执行 Transpose8xN 后再执行 TransposeNx8
那么输出与原始输入完全一致

### 需求：多流并行前缀和
SIMD 前缀和原语必须支持同时对 K 个独立流做 exclusive prefix sum，输入输出均为交错布局。

#### 场景：K=8 流同时前缀和
给定 8 个长度为 G 的 uint32_t 数组，已转置为交错布局
当调用 `ExclusivePrefixSumMulti(in, out, G, 8)`
那么对每个流 k（0 ≤ k < 8）：
  `out[0 * 8 + k] == 0`
  `out[j * 8 + k] == in[0*8+k] + in[1*8+k] + ... + in[(j-1)*8+k]` 对 j > 0

#### 场景：K < 8 流
给定 K=4 个流的交错输入（高 4 条 lane 为零）
当调用 `ExclusivePrefixSumMulti(in, out, G, 4)`
那么前 4 条 lane 的前缀和结果正确
且高 4 条 lane 的输出全为零

#### 场景：与单流前缀和结果一致
给定 K 个独立数组
当分别对每个数组调用 `ExclusivePrefixSum` 得到 K 个结果
且将 K 个数组转置后调用 `ExclusivePrefixSumMulti` 得到交错结果
且将交错结果反转置
那么两种方式的输出完全一致

### 需求：多流解码管线
`DecodeBatchBlocks` 必须使用多流管线解码常规块，每批最多 8 个块并行。

#### 场景：多流解码正确性
给定一个包含 N 条记录的集群，产生 M 个地址块（M > 1）
当通过多流管线解码
那么解码的 AddressEntry 序列与逐块 `DecodeSingleBlock` 串行解码的结果完全一致

#### 场景：批次边界处理
给定 num_regular_blocks = 17（2 个完整批次 + 1 个不完整批次）
当多流管线解码
那么前 8 个块作为第一批处理
且接下来 8 个块作为第二批处理
且最后 1 个块作为第三批（1 个有效流 + 7 个零填充 lane）处理
且所有块的解码结果正确

#### 场景：不足 8 块的批次
给定 num_regular_blocks = 3
当多流管线解码
那么 3 个块作为一批处理（3 个有效流 + 5 个零填充 lane）
且解码结果正确

#### 场景：尾块仍单独解码
给定一个最后地址块短于 `block_granularity` 的集群
当多流管线解码
那么常规块通过多流路径解码
且尾块通过 `DecodeSingleBlock` 单独解码
且两部分的结果正确拼接

### 需求：版本化格式强制
`.clu` 文件版本必须升级以反映 base_offsets 类型变更。

#### 场景：写入器使用版本 4
当写入器创建新的 `.clu` 文件
那么文件头版本字段为 `4`
且集群地址结尾部分的 `base_offsets[M]` 使用 `u32` 编码

#### 场景：读取器接受版本 4
给定一个版本 `4` 的 `.clu` 文件
当读取器打开文件
那么它正确解析 `u32` 格式的 `base_offsets[M]`

#### 场景：读取器拒绝版本 3
给定一个版本 `3` 的 `.clu` 文件
当版本 4 读取器打开文件
那么它返回不支持格式错误

## 变更的需求

### 需求变更：base_offsets 磁盘类型（来自 address-column-storage 规格）
原需求：`base_offsets[M] : u64[M]`
新需求：`base_offsets[M] : u32[M]`
原因：页索引不超过 uint32_t 范围（16 TB 寻址 @ page_size=4096），收窄后使多流 SIMD 核心循环全程 uint32。

### 需求变更：解码路径（来自 address-column-storage 规格）
原需求：常规块通过批解码路径解码多个块
新需求：常规块通过**多流垂直并行 SIMD** 路径解码，每批最多 8 个块共享一个融合的 PrefixSum+Materialize 循环
原因：多流模型比逐块串行的水平 scan 效率更高（约 50% 操作量减少）。
