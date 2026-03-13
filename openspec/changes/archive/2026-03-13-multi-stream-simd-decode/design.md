# 设计：多流 SIMD 地址列解码

## 概述
当前 `DecodeBatchBlocks` 对 K 个常规块逐个调用 `DecodeSingleBlock`，每次调用内部执行：
1. `BitUnpack(packed, bw, sizes, G)` — SIMD 在单流内水平展开
2. `ExclusivePrefixSum(sizes, prefix, G)` — SIMD 在单流内水平扫描
3. 标量循环物化 `AddressEntry`

本变更将步骤 2-3 替换为**多流垂直并行**模型：SIMD 寄存器的每条 lane 驱动一个独立块的前缀和流。K=8 个块同时推进，前缀和与物化融合为一个循环。

## 当前状态

### 解码管线（现今）
```
DecodeBatchBlocks(layout, blocks, total_records):
  for batch_start = 0; batch_start < num_regular; batch_start += 4:
    for i = 0..min(4, remaining)-1:
      DecodeSingleBlock(layout, blocks[batch_start+i], G, out):
        ┌─ BitUnpack(packed, bw, sizes, G)        // SIMD: 水平
        ├─ ExclusivePrefixSum(sizes, prefix, G)    // SIMD: 水平
        └─ for j=0..G-1:                           // 标量
             out[j].offset = (base + prefix[j]) * ps
             out[j].size   = sizes[j] * ps
```

### 问题
1. SIMD 仅在单个块内做水平 scan，不跨块并行。
2. `base_offset` 为 `uint64_t`，但实际是页索引（单文件 < 16TB），导致物化阶段需要 64-bit 运算，而 AVX2 缺乏 `mullo_epi64`。
3. 物化阶段是标量循环，未利用 SIMD。
4. 每个块的 BitUnpack → PrefixSum → Materialize 全串行，CPU 乱序引擎只能有限地利用 ILP。

## 核心算法：多流垂直并行前缀和

### 原理
单流水平 scan 在一个 __m256i 内做移位+加法+跨 lane 进位（IncPrefixSum8 + IncToExcl8），每 8 个元素需要约 6 条 SIMD 指令。

多流垂直方案：每条 lane 是一个独立的前缀和流。8 个流的第 j 个元素打包在一个 __m256i 中。循环 G 次，每次一条 `_mm256_add_epi32`——没有 lane 间依赖。

```
单流水平（当前）:              多流垂直（目标）:

  __m256i = [a0 a1 a2 ... a7]   __m256i = [s0[j] s1[j] ... s7[j]]
  内部有依赖链:                   lane 间无依赖:
  a1 依赖 a0                     s0[j] 只依赖 s0[j-1]
  a2 依赖 a0+a1                  s1[j] 只依赖 s1[j-1]
  ...                             ...
  需要 scan + carry              只需 running += vals
```

### 操作量对比
```
                     当前 (K=8 串行)      多流垂直 (K=8)
BitUnpack            8 × unpack(G)        8 × unpack(G)        相同
Transpose            无                    ~192 SIMD ops        新增
PrefixSum            8 × scan(G)          G × add(8-wide)
                     ≈ 384 ops            = 64 ops              6× ↓
Materialize          8 × G 标量           融合在 PrefixSum       ↓
                     = 512 ops            = 0 额外 ops
Deinterleave         无                    ~192 SIMD ops        新增
─────────────────────────────────────────────────────────────────
总计（不含 unpack）   ~896 ops             ~448 ops              ~50% ↓
```

## 数据布局

### 交错缓冲区（Interleaved Buffer）
多流内核要求同一元素索引 `j` 下所有 K 个流的值连续存放：

```
SoA（BitUnpack 输出）:                交错（PrefixSum 输入）:
  stream 0: [a0 a1 a2 ... a_{G-1}]     elem 0: [a0 b0 c0 ... h0]
  stream 1: [b0 b1 b2 ... b_{G-1}]     elem 1: [a1 b1 c1 ... h1]
  ...                               →   ...
  stream 7: [h0 h1 h2 ... h_{G-1}]     elem G-1: [a_{G-1} ... h_{G-1}]

内存布局:
  interleaved[j * K + k] = stream_k[j]
  一次 _mm256_load_si256(interleaved + j*8) 取 8 个流的第 j 个值
```

### 8×8 块转置
SoA→交错转置本质上是 K×G 矩阵转置。以 8×8 子块为单位，使用 AVX2 `_mm256_unpacklo/hi_epi32` + `_mm256_permute2x128_si256` 完成。G=64 时需 64/8 = 8 个子块。

## `base_offset` 类型收窄

### 动机
`base_offset` 存储的是**页索引**（page index），即 `byte_offset / page_size`。

```
uint32_t 页索引最大寻址:
  2^32 × 4096 = 16 TB（page_size=4096）
  2^32 × 1    = 4 GB（page_size=1，字节寻址极端情况）
```

对于本项目的 DataFile，16 TB 寻址完全足够。

### 影响链
```
  AddressBlock::base_offset : uint64_t → uint32_t    ← 内存表示
  .clu trailer base_offsets[M] : u64[M] → u32[M]     ← 磁盘格式
  EncodeSingleBlock 返回值                              ← 编码器
  DecodeSingleBlock/多流内核 输入                       ← 解码器
  AddressEntry::offset : uint64_t                      ← 不变（字节偏移）
```

展宽只发生在最终写回 `AddressEntry` 时：
```cpp
out[k*G+j].offset = static_cast<uint64_t>(base_u32 + prefix_u32) * page_size;
```

### 对核心循环的收益
全程 uint32 → AVX2 8 lanes 满载 → 无需拆成 2×__m128i 做 64-bit 运算。

## 解码管线（目标）

```
DecodeBatchBlocksMultiStream(layout, blocks, total_records):

  K = 8  // SIMD lane 数
  G = block_granularity  // 每块记录数

  for batch_start = 0; batch_start < num_regular; batch_start += K:
    actual_K = min(K, num_regular - batch_start)

    ┌─ Phase 1: BitUnpack (独立，actual_K 次调用)
    │  for k = 0..actual_K-1:
    │    BitUnpack(blocks[batch_start+k].packed, bw, sizes_soa[k], G)
    │
    ├─ Phase 2: Transpose SoA → Interleaved
    │  TransposeSoAToInterleaved(sizes_soa, sizes_interleaved, actual_K, G)
    │  // 如果 actual_K < K，剩余 lane 填零
    │
    ├─ Phase 3: 多流 PrefixSum + Materialize（融合循环）
    │  __m256i running = _mm256_setzero_si256()
    │  __m256i bases   = load_u32(base_offsets[batch_start..+K])
    │
    │  for j = 0..G-1:
    │    __m256i vals = load(sizes_interleaved + j*K)
    │    // 存储当前 running（exclusive prefix）
    │    store(prefix_interleaved + j*K, running)
    │    // 推进
    │    running = _mm256_add_epi32(running, vals)
    │
    ├─ Phase 4: Deinterleave + 写回 AddressEntry
    │  for k = 0..actual_K-1:
    │    for j = 0..G-1:
    │      uint32_t off_pages = bases_arr[k] + prefix_per_block[k][j]
    │      out[idx].offset = uint64_t(off_pages) * page_size
    │      out[idx].size   = uint32_t(sizes_soa[k][j]) * page_size
    └─

  // 尾块单独解码
  if has_tail:
    DecodeSingleBlock(layout, blocks.back(), tail_count, tail_out)
```

### Phase 3 细节：为什么可以融合
前缀和的输出 `running` 在加 `vals` 之前就是 exclusive prefix。物化需要的是 `base + running`。两者在同一循环迭代中可得，无需额外 pass。

### Phase 4 细节：deinterleave 选项
交错的 prefix 输出可以用两种方式消费：

**选项 A：先 8×8 反转置再顺序写回**
```
TransposeInterleavedToSoA(prefix_interleaved, prefix_soa, K, G)
for k: for j: out[k*G+j].offset = (bases[k] + prefix_soa[k][j]) * ps
```

**选项 B：直接从交错缓冲区 stride 读取**
```
for k: for j:
  out[k*G+j].offset = (bases[k] + prefix_interleaved[j*K+k]) * ps
```

选项 B 更简单但缓存不友好（stride-K 访问）。推荐选项 A。

## 新增 SIMD 原语

### `ExclusivePrefixSumMulti`
```cpp
/// 对 K 个独立流同时做 exclusive prefix sum。
///
/// 输入/输出均为交错布局：interleaved[j * num_streams + k] = stream_k[j]
///
/// @param interleaved_in   交错输入，G × K 个 uint32_t
/// @param interleaved_out  交错输出，G × K 个 uint32_t（可与 in 不同）
/// @param count            每流的元素数 G
/// @param num_streams      流数 K（必须 ≤ 8；如 < 8，高 lane 忽略）
void ExclusivePrefixSumMulti(const uint32_t* VDB_RESTRICT interleaved_in,
                              uint32_t* VDB_RESTRICT       interleaved_out,
                              uint32_t                     count,
                              uint32_t                     num_streams);
```

AVX2 实现：
```
running = _mm256_setzero_si256()
for j = 0..count-1:
    vals = _mm256_loadu_si256(in + j * 8)
    _mm256_storeu_si256(out + j * 8, running)
    running = _mm256_add_epi32(running, vals)
```

当 `num_streams < 8` 时，高位 lane 的输入为零，输出也为零，不影响正确性。

### `Transpose8xN` / `TransposeNx8`
```cpp
/// 将 K 个长度为 N 的 uint32_t 数组（SoA）转置为 N×K 交错布局。
/// K 必须 ≤ 8；不足 8 的 lane 用零填充。
void Transpose8xN(const uint32_t* const* streams_in,
                   uint32_t*             interleaved_out,
                   uint32_t              num_streams,
                   uint32_t              count_per_stream);

/// 逆操作：N×8 交错 → 8 个长度为 N 的数组。
void TransposeNx8(const uint32_t*  interleaved_in,
                   uint32_t* const* streams_out,
                   uint32_t         num_streams,
                   uint32_t         count_per_stream);
```

## 磁盘格式变更

### `.clu` 集群地址结尾部分
```
当前（版本 3）:                    新（版本 4）:
  page_size          : u32           page_size          : u32
  bit_width          : u8            bit_width          : u8
  block_granularity  : u32           block_granularity  : u32
  fixed_packed_size  : u32           fixed_packed_size  : u32
  last_packed_size   : u32           last_packed_size   : u32
  num_address_blocks : u32           num_address_blocks : u32
  base_offsets[M]    : u64[M]  →     base_offsets[M]    : u32[M]    ← 变更
  trailer_size       : u32           trailer_size       : u32
  block_magic        : u32           block_magic        : u32
```

节省：每集群 `4 × M` 字节（M = 地址块数）。

### 版本控制
- 版本 `4`：解析新的 u32 base_offsets 格式。
- 版本 `3`：拒绝，显示不支持格式错误。
- 版本 `2`：拒绝。

## 内存模型变化

### `AddressBlock`
```cpp
struct AddressBlock {
    uint32_t base_offset = 0;        // 从 uint64_t 变更为 uint32_t（页索引）
    std::vector<uint8_t> packed;     // 不变
};
```

### `AddressColumn`
新增多流解码 API，保留现有单块解码供尾块使用：
```cpp
class AddressColumn {
public:
    static constexpr uint32_t kMultiStreamWidth = 8;  // AVX2 lane 数

    // 新增：多流批解码（替代原 DecodeBatchBlocks 的常规块路径）
    static void DecodeMultiStream(const AddressColumnLayout& layout,
                                   const std::vector<AddressBlock>& regular_blocks,
                                   uint32_t num_regular_blocks,
                                   AddressEntry* out_entries);

    // 保留：单块解码（用于尾块）
    static void DecodeSingleBlock(...);

    // 重写：DecodeBatchBlocks 内部调用 DecodeMultiStream
    static Status DecodeBatchBlocks(...);
};
```

## 边界情况处理

### `num_regular_blocks < 8`
- 不足 8 个流时，`base_offsets` 和 `sizes_soa` 的高位 lane 填零。
- 多流内核正常运行，高位 lane 的前缀和结果为全零。
- Deinterleave 阶段只写回 `actual_K` 个流的结果。

### `num_regular_blocks == 0`
- 直接走尾块路径，不进入多流内核。

### `block_granularity` 不是 8 的倍数
- BitUnpack 已经做了 8 对齐的填充。
- 多流前缀和只循环 `block_granularity` 次，不超界。

## 测试策略

### SIMD 原语测试
1. `ExclusivePrefixSumMulti`：K=1,2,4,8 流 × 各种 count 值，与逐流 `ExclusivePrefixSum` 结果对比。
2. `Transpose8xN` / `TransposeNx8`：往返测试（transpose → inverse → 比对原始数据）。

### 地址列测试
1. 多流解码往返：Encode → DecodeMultiStream → 与原始 entries 对比。
2. 与 DecodeSingleBlock 串行结果的一致性测试。
3. 各种 num_blocks 值：1, 7, 8, 9, 16, 17（覆盖 <K, =K, >K, 跨批次边界）。
4. `base_offset` uint32_t 边界值测试（接近 2^32-1 的页索引）。

### 存储集成测试
1. `.clu` 版本 4 写入/读取往返。
2. 版本 3 文件被新读取器拒绝。
3. 端到端 Segment 测试。

## 风险
1. 8×8 转置开销在 G 较小时可能不划算——当 `num_regular_blocks < 3` 时可回退到串行路径。
2. `base_offset` 收窄为 uint32_t 后，page_size=1 的字节寻址模式最大只能寻址 4GB。需要确认项目中不存在 page_size=1 + 大文件的场景。
3. 多流内核输出为交错布局，反转置 + 写回 AddressEntry（AoS 结构含 uint64_t offset + uint32_t size）涉及不规则内存写入，需注意缓存友好性。
