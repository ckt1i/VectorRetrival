## Context

这条 change 的阻塞点已经明确：现有 `AddressColumn` 的编码语义不支持 redundant assignment 共享地址。

当前 V8 `.clu` 地址区的本质是：

- 每个 address block 只存一个 `base_offset`
- 每条记录只存 `size`
- 读取时通过 `ExclusivePrefixSum(sizes)` 恢复 offset

因此它要求 cluster 内地址满足近似：

```text
offset[i+1] = offset[i] + size[i]
```

这对单归属、按 cluster 连续写 `.dat` 的旧路径成立；对“全局只写一次 `data.dat`，多个 cluster 共享地址”的 redundant assignment 路径不成立。

另一方面，当前 serving 路径已经主要依赖：

- `.clu` 整体 preload
- `ParseClusterBlockView()`
- resident cluster view

而不是逐 address block 解压。既然地址解压已不再是主要路径，最直接的修法就是把 `.clu` 地址区改成固定宽度的原始地址表，并在查询阶段直接访问该表。

## Goals / Non-Goals

**Goals**
- 引入能表达任意地址列表的 `AddressColumn V2`
- 让 `.clu` 同时支持：
  - single-assignment
  - redundant_top2
  - shared `data.dat` address semantics
- 让 preload / parse path 不再依赖地址解压
- 保留旧 `.clu` 读取兼容，避免已有索引全部失效
- 用新格式重新完成四条 warm serving 对照实验

**Non-Goals**
- 不修改 `data.dat` 主体格式
- 不重新设计 raw vector / payload record
- 不在本次 change 中移除旧 V8 `.clu` 读取能力
- 不在本次 change 中继续压缩 V2 地址表

## Decisions

### 1. 引入 `AddressColumn V2`：原始地址表，而不是压缩地址块

**Decision**
- 新格式下，每条 posting 直接存一条原始地址项：

```cpp
struct RawAddressEntryV2 {
    uint32_t offset_pages;
    uint32_t size_pages;
};
```

- 地址以 page unit 存储，默认 page size 为 `4096`。

**Rationale**
- 固定宽度、无解压、支持任意 gap。
- 比直接存 byte-level `uint64 + uint32` 更紧凑。
- 与当前 `data.dat` 的 4KB 对齐语义一致。

**Alternatives considered**
- 扩展旧格式为 `delta_offset + size`  
  否决：仍需要解压与多流 SIMD 处理，复杂度高于当前需要。
- 直接存 byte-level `(uint64 offset, uint32 size)`  
  否决：可行，但在已知 4KB 对齐前提下不如 page-unit 紧凑。

### 2. `.clu` 升版本，V8/V9 双读兼容

**Decision**
- 将新的 `.clu` 文件版本升为 `V9`。
- `V8` 保持旧的压缩地址列。
- `V9` 使用原始地址表 `AddressColumn V2`。

**Rationale**
- 这是格式级变更，直接升版本最清晰。
- 避免在同一 version 下混入两个不同 address trailer 语义。

### 3. `ParsedCluster` 主路径改为 raw address view

**Decision**
- 新格式下，`ParsedCluster` 不再以 `decoded_addresses` 作为主热路径字段。
- 改为保留：
  - `const RawAddressEntryV2* raw_addresses`
  - `uint32_t address_page_size`

**Rationale**
- preload 后直接按 view 访问即可，无需再 materialize 一个 `std::vector<AddressEntry>`。
- 降低 parse/preload 开销，也更符合新格式的目的。

**Alternatives considered**
- 仍然在 parse 时全部 decode 成 `decoded_addresses`
  否决：可以保留作为兼容层，但不应继续作为主热路径。

### 4. 先完成格式升级与 correctness，再跑完整 benchmark

**Decision**
- 实现分两段：
  - 先做 V9 writer/reader/query correctness
  - 再跑四条 warm protocol

**Rationale**
- 不先把格式跑通，实验结果没有解释意义。

## New `.clu` V9 Format

### Cluster Block Layout

```text
cluster block
├── region 1: FastScan blocks
│   └── num_fastscan_blocks * fastscan_block_bytes
├── region 2: ExRaBitQ entries
│   └── num_records * exrabitq_entry_size
├── region 3: Raw Address Table V2
│   └── num_records * address_entry_size
└── region 4: Mini Trailer V2
```

### Raw Address Entry

```cpp
struct RawAddressEntryV2 {
    uint32_t offset_pages;   // addr.offset / page_size
    uint32_t size_pages;     // addr.size   / page_size
};
```

### Mini Trailer V2

建议字段：

```cpp
struct ClusterMiniTrailerV2 {
    uint32_t address_format_version;   // = 2
    uint32_t page_size;                // usually 4096
    uint32_t address_entry_size;       // sizeof(RawAddressEntryV2) = 8
    uint32_t num_address_entries;      // == num_records

    uint32_t address_payload_offset;   // relative to block start
    uint32_t address_payload_bytes;    // num_records * address_entry_size

    uint32_t mini_trailer_size;
    uint32_t block_magic;
};
```

## Code Changes

### A. `include/vdb/storage/address_column.h` / `src/storage/address_column.cpp`

当前 `AddressColumn` 以编码/解码为中心。要改成双格式支持：

- 保留 V1 旧接口，供 V8 reader 使用
- 增加 V2 数据结构和辅助函数，建议包括：
  - `RawAddressEntryV2`
  - `EncodeRawTableV2(const std::vector<AddressEntry>&, uint32_t page_size)`
  - `DecodeRawTableV2(...)` 或 lightweight materialize helper
  - `Validate4KAligned(...)`

这一步的目标不是“继续压缩”，而是把 address table 变成标准化原始表示。

### B. `include/vdb/storage/cluster_store.h` / `src/storage/cluster_store.cpp`

这是主改动点。

需要做的事情：

1. 升全局 `kFileVersion` 到 `9`
2. writer 分支：
   - V9 cluster block 写入 raw address table
   - V9 mini trailer 写入新字段
3. reader 分支：
   - `Open()` 识别 V8/V9
   - `EnsureClusterLoaded()` 对 V8 走旧 decode
   - 对 V9 直接读取 raw address payload
4. `ParseClusterBlockView()`：
   - V8 保持旧逻辑
   - V9 直接解析 raw address table view，不再 decode packed blocks
5. `ResidentClusterView`：
   - 增加 raw address view 支持

### C. `include/vdb/query/parsed_cluster.h`

需要扩展为双路径结构：

- 保留 `decoded_addresses` 兼容旧测试和旧 reader
- 增加：

```cpp
const storage::RawAddressEntryV2* raw_addresses = nullptr;
uint32_t address_page_size = 0;
bool addresses_are_raw_v2 = false;
```

并提供一个统一 accessor，类似：

```cpp
AddressEntry AddressAt(uint32_t i) const;
```

这样 query 代码可以逐步从 `decoded_addresses[i]` 迁移出去。

### D. `src/query/overlap_scheduler.cpp`

当前 dedup 逻辑按 `addr.offset` 去重。新格式下语义不变，但获取地址的方式要从：

```cpp
pc.decoded_addresses[idx]
```

迁移为：

```cpp
pc.AddressAt(idx)
```

这样 V8/V9 都能共用一条 query 逻辑。

### E. `src/index/ivf_builder.cpp`

builder 不再需要围绕“地址连续编码”做妥协。

在 V9 设计下：

- 可继续保留共享 `data.dat` 语义
- 不需要为 V2 地址表对 cluster members 做“连续布局排序”
- 只需要确保：
  - `code[i]`
  - `address[i]`
  - `vec_id[i]`
  - `CRC/local ids[i]`
  同序即可

也就是说，`sorted_members` 从“为了满足地址列编码”转变为“仅在需要稳定顺序时使用”。如果没有其他需求，cluster members 可直接按现有 posting 顺序写入。

### F. `benchmarks/bench_e2e.cpp`

新增或补充输出：

- `clu_format_version`
- `address_format`
- `assignment_mode`
- `assignment_factor`
- duplication / dedup stats

这样能区分：
- 旧 `.clu` single baseline
- 新 `.clu` single baseline
- 新 `.clu` redundant_top2

## Reader / Query Migration Strategy

### V8

```text
ParseClusterBlockView
  -> parse packed address blocks
  -> DecodeBatchBlocks
  -> fill decoded_addresses
```

### V9

```text
ParseClusterBlockView
  -> parse raw address payload location
  -> set raw_addresses view
  -> no address decode
```

### Query Access

统一改成：

```text
ParsedCluster::AddressAt(i)
```

内部：
- V9: `raw_addresses[i] -> page_size multiply`
- V8: `decoded_addresses[i]`

## Validation Plan

### Format-level

1. V9 writer/reader roundtrip
- 写入 single cluster
- 读回 raw address table
- 验证每项 `(offset,size)` 一致

2. V9 supports gapped addresses
- 构造有 gap 的地址序列
- 验证 V9 可正确 roundtrip
- 证明这是 V1 无法表示的情况

3. V8 compatibility
- 旧测试继续通过
- reader 仍能读取已有 V8 `.clu`

### Query-level

1. `ParsedCluster::AddressAt()` on V8/V9
- 两种格式返回相同语义的 `AddressEntry`

2. redundant_top2 sanity
- 小数据集上不再出现 catastrophic recall collapse

## Experiment Plan

### Phase 0: format sanity

- single small synthetic index, V9
- redundant small synthetic index, V9
- verify address roundtrip / dedup / recall sanity

### Phase A: main warm comparison

固定：
- dataset: `coco_100k`
- queries: `1000`
- `bits=4`
- `epsilon=0.90`
- `clu_mode=full_preload`

Runs:
- `2048 single` (V9)
- `2048 redundant_top2` (V9)

### Phase B: secondary warm comparison

Runs:
- `1024 single` (V9)
- `1024 redundant_top2` (V9)

### Decision

- 若 `2048 redundant_top2` 在 `0.99+ recall` 区间优于 `2048 single` 且不丢掉对 `1024` 的 latency 优势，则保留
- 若 `1024 redundant_top2` 只是继续放大 total probed / latency，则不作为主线推进

## Risks / Trade-offs

- [风险] V9 raw address table 增大 `.clu` 大小  
  → Mitigation：输出 preload bytes / index size，对比冗余带来的收益。

- [风险] V8/V9 双读兼容增加 reader 复杂度  
  → Mitigation：统一抽象到 `ParsedCluster::AddressAt()`，将复杂度限制在 parse 层。

- [风险] query 热路径如果频繁做 page-size 乘法，可能引入轻微 CPU 开销  
  → Mitigation：这是常数级转换，通常远小于原先地址解压；如有必要可在 preload 时 materialize。

## Open Questions

- V9 single baseline 是否需要完全替换现有 warm baseline，还是先与 V8 single 做一次 sanity 对比
- `raw_addresses` 是否在 resident preload 中直接保留 view，还是复制到独立 buffer
- 是否需要在 `segment.meta` 中额外记录 `clu_format_version` 方便 benchmark 输出
