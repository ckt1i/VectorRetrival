目前这个仓库的代码是从github上扒下来的，我现在有这样一份架构文档，和以json格式保存的该仓库从建立到目前为止的讨论过程，请你先查看下面的文档和历史对话，给出后续的开发计划：


## Plan: Complete Handoff Architecture Document

Below is the full architecture specification for the **VDB VectorRetrival** project — a high-performance vector search engine with columnar payload storage, using IVF + ConANN + RaBitQ.

---

# VDB VectorRetrival — 完整架构设计文档

## 1. 系统总览

**VDB VectorRetrival** 是一个面向高维向量近似最近邻搜索 (ANNS) 的存储与查询引擎。核心设计理念：

- **IVF + ConANN + RaBitQ** 查询流水线：IVF 粗聚类 → ConANN 三分类 → RaBitQ 量化距离估算 → Rerank 精排
- **Cluster 1:1 DataFile**：每个 IVF 聚类对应一个独立的 DataFile，存储原始向量 + payload 列存数据
- **Address Column**：通过 `base_offset + bit-packed sizes` 编码每条记录在 DataFile 中的物理地址，支持 SIMD 解码
- **Streaming Rerank**：Producer-Consumer 模式，Rerank 不等待所有 I/O 完成，流式消费已完成的读取
- **io_uring**：异步 I/O（初期单线程单 ring，预留多线程多 ring 扩展点）

**设计约束**：C++17, AVX2/AVX-512, 批量离线构建（无 WAL），单线程查询初期版本

---

## 2. 目录结构

### 2.1 当前目录结构（Phase 0–2 已完成）

```
VectorRetrival/
├── CMakeLists.txt                      # 主构建配置
├── cmake/
│   └── FlatBuffersGenerate.cmake       # FlatBuffers 代码生成
├── schema/
│   ├── columns.fbs                     # 列存元数据 schema
│   ├── segment_meta.fbs               # Segment 元数据 schema
│   └── record.fbs                      # 记录文件 schema
├── include/vdb/
│   ├── common/
│   │   ├── types.h                     # 核心类型定义
│   │   ├── status.h                    # 错误处理 Status/StatusOr
│   │   ├── aligned_alloc.h            # SIMD 对齐内存分配
│   │   └── macros.h                    # 编译器宏
│   └── columns/
│       ├── column_chunk_writer.h      # 单列 chunk 写入
│       ├── column_chunk_reader.h      # 单列 chunk 读取
│       └── column_store.h             # 多列管理器
├── src/columns/
│   ├── column_chunk_writer.cpp
│   ├── column_chunk_reader.cpp
│   └── column_store.cpp
└── tests/
    ├── CMakeLists.txt
    ├── common/ (3 test files)
    ├── schema/ (2 test files)
    └── columns/ (2 test files)
```

### 2.2 目标目录结构（所有 Phase 完成后）

```
VectorRetrival/
├── CMakeLists.txt
├── cmake/FlatBuffersGenerate.cmake
├── schema/
│   ├── columns.fbs                  # ✏️ 需修改：增加 RaBitQ 编码参数
│   ├── segment_meta.fbs            # ✏️ 需重构：RaBitQ/ConANN/ClusterMeta
│   ├── record.fbs                   # ✏️ 需修改：增加 DataFileHeader
│   └── address.fbs                  # 🆕 AddressBlock 元数据
├── include/vdb/
│   ├── common/
│   │   ├── types.h                  # ✏️ 需修改：新增类型
│   │   ├── status.h                 # ✅ 不变
│   │   ├── aligned_alloc.h         # ✅ 不变
│   │   └── macros.h                 # ✅ 不变
│   ├── columns/                     # ✅ 不变（被 ClusterStore 复用）
│   │   ├── column_chunk_writer.h
│   │   ├── column_chunk_reader.h
│   │   └── column_store.h
│   ├── simd/                        # 🆕 Phase 3
│   │   ├── distance_l2.h           # L2 距离 AVX2/AVX-512
│   │   ├── bit_unpack.h            # bit-unpack SIMD 内核
│   │   └── prefix_sum.h            # 前缀和 SIMD
│   ├── codec/                       # 🆕 Phase 3
│   │   ├── bitpack_codec.h         # 位压缩编解码
│   │   └── dict_codec.h            # 字典编解码
│   ├── rabitq/                      # 🆕 Phase 4
│   │   ├── rabitq_encoder.h        # RaBitQ 量化编码
│   │   ├── rabitq_estimator.h      # RaBitQ 距离估算
│   │   └── popcount.h              # popcount 工具
│   ├── storage/                     # 🆕 Phase 5
│   │   ├── address_column.h        # 地址列编解码
│   │   ├── cluster_store.h         # 单 Cluster 存储
│   │   ├── data_file_writer.h      # DataFile 写入
│   │   ├── data_file_reader.h      # DataFile 读取
│   │   └── segment.h               # Segment 管理
│   ├── index/                       # 🆕 Phase 6
│   │   ├── ivf_index.h             # IVF 索引
│   │   ├── ivf_builder.h           # IVF 离线构建
│   │   └── conann.h                # ConANN 三分类
│   └── query/                       # 🆕 Phase 7
│       ├── io_uring_reader.h       # io_uring 异步读取
│       ├── read_queue.h            # 双优先级读取队列
│       ├── overlap_scheduler.h     # Probe+I/O 重叠调度
│       ├── rerank_consumer.h       # 流式 Rerank 消费者
│       ├── warmup.h                # Centroid 预热
│       ├── result_collector.h      # Top-K 结果收集
│       └── search_context.h        # 查询上下文
├── src/
│   ├── columns/                     # ✅ 不变
│   ├── simd/                        # 🆕
│   ├── codec/                       # 🆕
│   ├── rabitq/                      # 🆕
│   ├── storage/                     # 🆕
│   ├── index/                       # 🆕
│   └── query/                       # 🆕
└── tests/
    ├── common/                      # ✅ 不变
    ├── schema/                      # ✅ 不变（可能需新增 schema 测试）
    ├── columns/                     # ✅ 不变
    ├── simd/                        # 🆕
    ├── codec/                       # 🆕
    ├── rabitq/                      # 🆕
    ├── storage/                     # 🆕
    ├── index/                       # 🆕
    └── query/                       # 🆕
```

---

## 3. 核心数据结构

### 3.1 已有类型（types.h）— 保留不变

| 类型 | 定义 | 用途 |
|------|------|------|
| `VecID` | `uint64_t` | 向量全局唯一标识 |
| `RowID` | `uint64_t` | 行号 |
| `SegmentID` | `uint32_t` | Segment 标识 |
| `ListID` | `uint32_t` | 倒排列表 ID |
| `Dim` | `uint32_t` | 向量维度 |
| `ColumnID` | `uint32_t` | 列标识 |
| `MetricType` | enum | L2/IP/Cosine |
| `DType` | enum | 数据类型 |
| `ColumnLocator` | struct | 列数据定位 (chunk_id, data_offset, data_length, offset_table_pos) |
| `ColumnSchema` | struct | 列定义 (id, name, dtype) |
| `Datum` | struct | 通用数据值 |
| `SearchResult` | struct | 搜索结果 (vec_id, distance) |

### 3.2 需新增到 types.h 的类型

```
FileID         = uint16_t          // DataFile 标识（1:1 cluster，保留供扩展）
ClusterID      = uint32_t          // 等同 ListID，语义更明确

ResultClass    = enum { SafeIn, SafeOut, Uncertain }
                                    // ConANN 三分类结果

AddressEntry   = struct {
    offset: uint64,                // 记录在 DataFile 中的字节偏移
    size:   uint32                 // 记录字节长度
}

ReadTaskType   = enum {
    ALL,                           // SafeIn 小记录: 整条读取
    FRONT,                         // SafeIn 大记录: 前半部分 (vec + partial payload)
    VEC_ONLY,                      // Uncertain: 只读向量
    BACK,                          // Rerank 确认后: 读取剩余 payload
    PAYLOAD                        // SafeIn 大记录的后半部分
}

ReadTask       = struct {
    cluster_id:  ClusterID,
    local_idx:   uint32,           // 在 cluster 内的记录索引
    addr:        AddressEntry,     // 解码后的物理地址
    task_type:   ReadTaskType,
    read_offset: uint64,           // 实际读取起始
    read_length: uint32,           // 实际读取长度
    priority:    uint8             // 0=Qv(高), 1=Qp(低)
}

CompletedRead  = struct {
    task:        ReadTask,
    buffer:      AlignedBuffer,    // 读取到的数据
    status:      Status
}

Candidate      = struct {
    vec_id:       VecID,
    approx_dist:  float,           // RaBitQ 估算距离
    result_class: ResultClass,
    cluster_id:   ClusterID,
    local_idx:    uint32
}

RaBitQConfig   = struct {
    bits:         uint8,           // 1/2/4/8, 默认 1
    block_size:   uint32,          // 默认 64
    c_factor:     float            // 默认 5.75, epsilon = c * 2^(-B/2) / sqrt(D)
}
```

### 3.3 需弃用 / 移除的类型

- `RecordPhysicalAddr`：被 `AddressEntry` 取代
- `PayloadMode` / `ExternRef`：DataFile 设计中 payload 统一内联于 ColumnStore，不再需要 inline/extern 分支

---

## 4. 存储布局

### 4.1 Cluster 内部布局（ClusterStore）

所有 Cluster 存储在一个 ClusterStore 文件中（`.clu`），包含一个内部查询各个聚类向量块的表；所有的量化向量和原始数据的簇统一打包成一个DataBlocks，并且按照查找表里排布原始向量顺序存储对应的簇数据
```
┌────────────────────────────────────────────────────────┐
| Cluster Header:metadata                                |
|   - magic, num_records, dimension,                     |
|   - rabitq_configurations,                             |
|   - etc..                                              |
|   - Cluster Table(Hybrid storage):                     |
|  ┌──────────────────┐┌──────────────────────────────┐  |
|  | Centroid_vector1 || Block_Address1 | Block_Size1 |  |
|  ├──────────────────┤├──────────────────────────────┤  |
|  | ...              ||                              |  |
|  ├──────────────────┤├──────────────────────────────┤  |
|  | Centroid_vectorN || Block_AddressN | Block_SizeN |  |
|  └──────────────────┘└──────────────────────────────┘  |
├────────────────────────────────────────────────────────┤
| Cluster Data Blocks:                                   |
|   Block1:                                              |
|  ┌─────────────────────────────────────────────┐       |
|  │ RaBitQ Quantized Vectors                    │       |
|  │   - B bits × D dims × N records             │       |
|  │   - 按 block_size(64)分块对齐                 |       |
|  ├─────────────────────────────────────────────┤       |
|  │ Address Column                              │       |
|  │   - AddressBlock[0..⌈N/64⌉-1]               │       |
|  │   - 每块: base_offset(u64) +                 │       |
|  │           bit-packed sizes(bit_width × 64)  │       |
|  ├─────────────────────────────────────────────┤       |
|  │ Address Trailer: metadatas in block         │       |
|  └─────────────────────────────────────────────┘       |
|   Block2:                                              |
|  ┌─────────────────────────────────────────────┐       |
|  | ...                                         |       |
|  └─────────────────────────────────────────────┘       |
|  ...                                                   |
└────────────────────────────────────────────────────────┘ 
```

### 4.2 DataFile 布局

DataFile（`.data`）存储原始向量 + payload 列存：

```
┌─────────────────────────────────────────────┐
│ DataFileHeader (FlatBuffers)                │
│   - magic, version                          │
│   - num_records, dimension                  │
│   - column_schemas[]                        │
│   - column_region_offsets[]                 │
├─────────────────────────────────────────────┤
│ Record 0: [raw_vector | col0 | col1 | ...] │
│ Record 1: [raw_vector | col0 | col1 | ...] │
│ ...                                         │
│ Record N-1: [...]                           │
└─────────────────────────────────────────────┘
```

**关键**：记录按行存储（非列存），每条记录的 `(offset, size)` 由 Address Column 编码。这样单条 pread 即可读取完整记录或其前/后半段。

### 4.3 Address Column 编码细节

Address Column 按照一定的粒度（4KB）对原始数据进行分页，并且 block 粒度（默认 64 条记录）编码：

```
AddressBlock:
  base_offset : uint64           // 块内第一条记录的 DataFile 字节偏移
  bit_width   : uint8            // 每个 size 值的位宽
  sizes[64]   : bit-packed       // 每条记录的字节长度，bit_width 位
  pagesize    : uint8            // 原始数据分页力度（默认按照4KB分页）

解码第 i 条记录的地址:
  block_idx  = i / 64
  local_idx  = i % 64
  block      = address_blocks[block_idx]
  sizes[]    = SIMD_bit_unpack(block.sizes, block.bit_width)
  prefix     = prefix_sum(sizes[0..local_idx-1])
  offset     = block.base_offset + prefix
  size       = sizes[local_idx]
```

**SIMD 解码流程**：`bit_unpack` (AVX2 展开) → `prefix_sum` (AVX2 scan) → 得到 `(offset, size)` 数组。整个 block 64 条一次解码。

### 4.4 Segment 结构

```
segment_<id>/
├── segment.meta                 # SegmentMeta (FlatBuffers)
├── cluster.clu                  # ClusterStore (Cebtoids + RaBitQ + address)
├── data.dat             # DataFile (raw vec + payload)
```

---

## 5. 查询流水线

### 5.1 总体流程

```
SearchRequest(query_vec, top_k, nprobe, payload_columns[])
  │
  ▼
Phase-A: Warmup (一次性)
  │  加载.clu的查询表到内存, mmap ClusterStore 头部
  │  预先计算RabitQ量化的误差，以及ConANN搜索的top_k门限：随机采样多个query得到top_k的特定分位数（默认99%）距离
Phase-B: Init
  │  计算 query 到所有 centroid 的距离
  │  选出 nprobe 个最近的 cluster
  │  对每个 cluster 执行 ConANN 三分类门限计算
  ▼
Phase-C: Probe + I/O + Streaming Rerank (交叉执行)
  │  ┌─ Probe: 逐 cluster 解码 RaBitQ → 计算 approx_dist
  │  │         → ConANN 分类 → 生成 ReadTask 入队
  │  │
  │  ├─ I/O:   从 Qv(高优先) / Qp(低优先) 取 ReadTask
  │  │         → io_uring 提交 pread → 完成后入 CompletedQueue
  │  │
  │  └─ Rerank: 从 CompletedQueue 消费
  │             → VEC_ONLY: L2精确距离 → 确认 SafeIn → 生成 BACK task
  │             → ALL/FRONT: 解析向量 + payload → 加入 TopK
  │             → BACK/PAYLOAD: 拼接 payload → 加入 TopK
  ▼
Phase-D: Result Assembly
  │  TopK 结果按距离排序
  │  返回 SearchResult[] (vec_id, distance, payload_columns)
```

### 5.2 ConANN 三分类规则

对于 cluster $c$，query $q$，centroid $o_c$，设 $d_c = \|q - o_c\|$：

- **SafeIn**: $d_{\text{approx}} < \tau_{\text{in}}$ — 一定在 Top-K 内，直接读取全部数据
- **SafeOut**: $d_{\text{approx}} > \tau_{\text{out}}$ — 一定不在 Top-K 内，跳过
- **Uncertain**: $\tau_{\text{in}} \le d_{\text{approx}} \le \tau_{\text{out}}$ — 需要精确距离验证

门限 $\tau$ 由 RaBitQ 误差界 $\epsilon = c \cdot 2^{-B/2} / \sqrt{D}$ 和当前 Top-K 堆顶距离动态确定。

### 5.3 I/O 分裂规则 (max_prefetch_size = 1MB)

| 分类 | 记录大小 | 行为 |
|------|----------|------|
| SafeIn | ≤ max_prefetch_size | 整条 → Qv (type=ALL) |
| SafeIn | > max_prefetch_size | 前半 → Qv (type=FRONT)，后半 → Qp (type=PAYLOAD) |
| Uncertain | 任意 | 向量部分 → Qv (type=VEC_ONLY) |
| Uncertain→确认SafeIn | 任意 | 剩余 payload → Qp (type=BACK) |
| SafeOut | — | 不发起 I/O |

### 5.4 双队列设计

```
Qv (Vector Queue)  — 高优先级, FIFO
  存放: ALL / FRONT / VEC_ONLY 类型的 ReadTask
  
Qp (Payload Queue) — 低优先级, FIFO
  存放: PAYLOAD / BACK 类型的 ReadTask

调度策略: Qv 非空时优先取 Qv; Qv 为空时取 Qp
         两个队列均 FIFO, 不需要排序

CompletedQueue — 完成队列, FIFO
  存放: CompletedRead (I/O 完成的读取结果)
  Rerank 线程从此队列消费
```

### 5.5 线程模型（初期 + 扩展点）

**初期：单线程**
- 一个线程依次执行 Probe batch → 提交 io_uring → poll completions → Rerank
- 使用协作式调度：Probe N 条 → 切换到 poll I/O → 切换到 Rerank 已完成项 → 继续 Probe

**扩展点**：
- `IoPool`：多线程 + 多 io_uring ring，每个线程独立 ring
- `RerankPool`：多线程精排，CompletedQueue 变为 MPMC 无锁队列
- `ProbePool`：多 cluster 并行 Probe

---

## 6. 已完成 Phase 的修改清单

### Phase 0: Common（✅ 已完成 — 需修改 1 个文件）

**types.h 修改**：
1. 新增类型定义：`ClusterID`, `FileID`, `ResultClass`, `AddressEntry`, `ReadTaskType`, `ReadTask`, `CompletedRead`, `Candidate`, `RaBitQConfig`（见 §3.2）
2. 弃用 `RecordPhysicalAddr`（添加 `[[deprecated]]` 注解或直接移除）
3. `SearchResult` 扩展：增加 `payload: std::map<ColumnID, Datum>` 字段，使搜索结果可携带 payload

**其余文件不变**：status.h、aligned_alloc.h、macros.h

### Phase 1: Schemas（✅ 已完成 — 3 个文件需修改 + 1 个新增）

**segment_meta.fbs 重构**：
1. 新增 `RaBitQParams` table：`bits:uint8`, `block_size:uint32`, `c_factor:float`, `codebook_offset/length`
2. 新增 `ConANNParams` table：`tau_in_factor:float`, `tau_out_factor:float`
3. 新增 `AddressBlockMeta` table：`base_offset:uint64`, `bit_width:uint8`, `data_offset:uint64`, `data_length:uint32`
4. 新增 `AddressColumnMeta` table：`block_granularity:uint32`, `num_blocks:uint32`, `blocks:[AddressBlockMeta]`
5. 新增 `ClusterMeta` table：`cluster_id:uint32`, `size:uint32`, `centroid_offset/length`, `rabitq_data_offset/length`, `address_column:AddressColumnMeta`, `data_file_path:string`, `norms_offset/length`
6. 在 `SegmentMeta` 中：用 `clusters:[ClusterMeta]` 替换 `inverted_lists:[InvertedListMeta]`；用 `rabitq_params:RaBitQParams` 替换 `pq_params`；新增 `conann_params:ConANNParams`
7. 可弃用 `PqParams`, `OpqParams`, `InvertedListMeta`（保留但标注弃用）

**record.fbs 修改**：
1. 新增 `DataFileHeader` table：`magic:uint32`, `version:uint32`, `cluster_id:uint32`, `num_records:uint64`, `dimension:uint32`, `column_schemas:[ColumnSchemaEntry]`, `column_region_offsets:[uint64]`, `checksum:uint32`
2. 新增辅助 `ColumnSchemaEntry` table：`column_id:uint32`, `name:string`, `data_type:DataType`（引用 columns.fbs 的 DataType）

**columns.fbs 小修**：
1. `EncodingType` 新增 `RABITQ = 8`
2. 新增 `RaBitQEncodingParams` table：`bits:uint8`, `block_size:uint32`
3. 在 `EncodingParams` union 中追加 `RaBitQEncodingParams`

**新增 address.fbs**（可选，如果 AddressBlock 元数据独立管理）：
- 定义 `AddressBlock`, `AddressColumnHeader` 等 table

### Phase 2: Column Store（✅ 已完成 — 不修改）

column_chunk_writer.h、column_chunk_reader.h、column_store.h 及其实现文件保持原样。这些组件将被 Phase 5 的 `DataFileWriter`/`ClusterStore` 直接复用：
- `ColumnChunkWriter` → DataFile 构建时写入 payload 列
- `ColumnChunkReader` → DataFile 读取时解析 payload 列
- `ColumnStore` → DataFile 内部的多列管理

---

## 7. 未完成 Phase 的实现计划

### Phase 3: SIMD + Codec 基础

| 文件 | 内容 |
|------|------|
| `include/vdb/simd/distance_l2.h` + `.cpp` | AVX2 L2 距离函数：`float l2_sqr(const float* a, const float* b, Dim d)`, AVX-512 分支可选 |
| `include/vdb/simd/bit_unpack.h` + `.cpp` | AVX2 bit-unpack 内核：将 `bit_width × 64` 的 packed 数据展开为 `uint32_t[64]` |
| `include/vdb/simd/prefix_sum.h` + `.cpp` | AVX2 前缀和：对 `uint32_t[64]` 计算 exclusive prefix sum |
| `include/vdb/codec/bitpack_codec.h` + `.cpp` | 位压缩编解码器：`Encode(values[], bit_width) → packed_bytes`, `Decode(packed, bit_width) → values[]`，调用 simd/bit_unpack |
| `include/vdb/codec/dict_codec.h` + `.cpp` | 字典编解码器：`BuildDict(values[]) → (dict, indices[])`, `Decode(indices[], dict) → values[]`，per-column 独立字典 |
| `tests/simd/` | distance_l2_test, bit_unpack_test, prefix_sum_test |
| `tests/codec/` | bitpack_codec_test, dict_codec_test |

### Phase 4: RaBitQ 量化

| 文件 | 内容 |
|------|------|
| `include/vdb/rabitq/rabitq_encoder.h` + `.cpp` | 量化编码：`Encode(raw_vec, dim, bits) → quantized_vec`; 支持 1/2/4/8 bit；生成 per-vector norm |
| `include/vdb/rabitq/rabitq_estimator.h` + `.cpp` | 距离估算：`Estimate(query_quantized, db_quantized, norms, dim, bits) → approx_dist`; 使用 popcount + lookup table |
| `include/vdb/rabitq/popcount.h` + `.cpp` | popcount 工具：AVX2 `_mm256_popcnt_epi8` 模拟 或 AVX-512 VPOPCNTDQ |
| `tests/rabitq/` | rabitq_encoder_test, rabitq_estimator_test, popcount_test |

### Phase 5: Storage 层

| 文件 | 内容 |
|------|------|
| `include/vdb/storage/address_column.h` + `.cpp` | Address Column 编解码：`Encode(entries[]) → AddressBlocks[]`（自动选 bit_width）；`DecodeBlock(block_idx) → AddressEntry[64]`（调用 SIMD bit_unpack + prefix_sum） |
| `include/vdb/storage/cluster_store.h` + `.cpp` | 单 Cluster 管理：写入 RaBitQ 向量 + Address Column；读取/解码 RaBitQ 向量和地址。内部使用 `ClusterMeta` (FlatBuffers) |
| `include/vdb/storage/data_file_writer.h` + `.cpp` | DataFile 写入：接受 `(raw_vector, payload_columns)` → 按行存储 → 记录 `(offset, size)` 列表交给 AddressColumn 编码 |
| `include/vdb/storage/data_file_reader.h` + `.cpp` | DataFile 读取：给定 `AddressEntry` → pread 整条/部分记录 → 解析为 `(raw_vec, payload_datums)` |
| `include/vdb/storage/segment.h` + `.cpp` | Segment 管理：管理所有 Cluster 的 `.clu` + `.dat` 文件；提供 `Open/Close`, `GetCluster(id)`, `GetDataFileReader(id)` |
| `tests/storage/` | address_column_test, cluster_store_test, data_file_test, segment_test |

### Phase 6: IVF 索引

| 文件 | 内容 |
|------|------|
| `include/vdb/index/ivf_builder.h` + `.cpp` | 离线构建：K-means 聚类 → 生成 centroids → 按 cluster 分配向量 → 调用 ClusterStore + DataFileWriter 写入 |
| `include/vdb/index/ivf_index.h` + `.cpp` | 查询时索引：加载 centroids → `Search(query, nprobe) → ClusterID[]` → 返回待探测 cluster 列表 |
| `include/vdb/index/conann.h` + `.cpp` | ConANN 三分类：`Classify(approx_dist, tau_in, tau_out) → ResultClass`；门限动态计算：基于 epsilon 和当前 TopK 堆顶 |
| `tests/index/` | ivf_builder_test, ivf_index_test, conann_test |

### Phase 7: 查询流水线

| 文件 | 内容 |
|------|------|
| `include/vdb/query/search_context.h` + `.cpp` | 查询上下文：持有 query_vec, top_k, nprobe, max_prefetch_size, TopK 堆, 各队列引用 |
| `include/vdb/query/io_uring_reader.h` + `.cpp` | io_uring 封装：`Init(queue_depth)`, `Submit(ReadTask[])`, `PollCompletions() → CompletedRead[]`；初期单 ring 单线程 |
| `include/vdb/query/read_queue.h` + `.cpp` | 双队列：`Qv` (高优先) + `Qp` (低优先)；`Push(ReadTask)`, `Pop() → ReadTask`（Qv 优先） |
| `include/vdb/query/overlap_scheduler.h` + `.cpp` | 调度主循环：协作式执行 Probe batch → Submit I/O → Poll completions → 入 CompletedQueue → Rerank 消费。控制 Probe / I/O / Rerank 交替 |
| `include/vdb/query/rerank_consumer.h` + `.cpp` | 流式 Rerank：从 CompletedQueue 取 CompletedRead → 精确 L2 距离 → 更新 TopK → Uncertain 确认后生成 BACK task 入 Qp |
| `include/vdb/query/warmup.h` + `.cpp` | 预热：加载 centroids 矩阵、mlock ClusterStore 头部元数据 |
| `include/vdb/query/result_collector.h` + `.cpp` | TopK 收集：max-heap (size=K), `TryInsert(vec_id, dist, payload)`, `Finalize() → sorted SearchResult[]` |
| `tests/query/` | io_uring_reader_test, read_queue_test, overlap_scheduler_test, rerank_consumer_test, result_collector_test |

### CMakeLists.txt 修改

需新增以下 library targets:
- `vdb_simd` (STATIC): simd/ 源文件
- `vdb_codec` (STATIC): codec/ 源文件，链接 vdb_simd
- `vdb_rabitq` (STATIC): rabitq/ 源文件，链接 vdb_simd
- `vdb_storage` (STATIC): storage/ 源文件，链接 vdb_columns, vdb_codec, vdb_schema
- `vdb_index` (STATIC): index/ 源文件，链接 vdb_storage, vdb_rabitq
- `vdb_query` (STATIC): query/ 源文件，链接 vdb_index, vdb_storage, vdb_simd，外部依赖 liburing

---

## 8. 依赖关系图

```
vdb_query
  ├── vdb_index
  │     ├── vdb_rabitq
  │     │     └── vdb_simd
  │     └── vdb_storage
  │           ├── vdb_columns
  │           ├── vdb_codec
  │           │     └── vdb_simd
  │           └── vdb_schema
  ├── vdb_simd
  └── liburing (external)

vdb_common (INTERFACE) ← 所有模块都依赖
```

---

### Further Considerations

1. **Address Column 是否需要独立 `.fbs` 文件？** 可以内嵌在 `segment_meta.fbs` 的 `ClusterMeta` 中（推荐），也可以独立为 `address.fbs` 以便复用——建议先内嵌，后续按需拆分。
2. **DataFile 内部是行存还是列存？** 当前设计为行存（每条记录连续 `[vec|col0|col1|...]`），这对单条 pread 读取友好；如果 payload 列需要大量列投影扫描，可考虑混合格式（vec 行存 + payload 列存分区），但会增加复杂度——建议 Phase 5 先实现行存，评估性能后再考虑。
3. **io_uring 是否有 fallback？** 建议在 `io_uring_reader.h` 中提供一个 `PreadFallbackReader` 实现相同接口，在不支持 io_uring 的内核（< 5.1）上降级为同步 pread，便于开发期测试。