# VDB VectorRetrival — 项目开发总结

> **最后更新**：2026-04-01
> **当前状态**：Phase 0–15 全部完成（含 v7 存储格式 + FastScan AVX-512）
> **下一步**：生产路径集成 FastScan Stage 1 到 ConANN 分类；运行时 ISA dispatch

---

## 目录

1. [项目概述](#1-项目概述)
2. [已完成模块一览](#2-已完成模块一览)
3. [Phase 0–2：基础设施（Common / Schema / ColumnStore）](#3-phase-02基础设施)
4. [Phase 3：SIMD + Codec](#4-phase-3simd--codec)
5. [Phase 4：RaBitQ 量化](#5-phase-4rabitq-量化)
6. [Phase 5：Storage 层](#6-phase-5storage-层)
7. [Phase 6：IVF 索引](#7-phase-6ivf-索引)
8. [Phase 7：查询流水线](#8-phase-7查询流水线)
9. [Phase 8：Payload 数据管道 + IO 加载器](#9-phase-8payload-数据管道--io-加载器)
10. [Phase 9：Early Stop 聚类探测提前终止](#10-phase-9early-stop-聚类探测提前终止)
11. [Phase 10：CRC 自适应 Early Stop](#11-phase-10crc-自适应-early-stop)
12. [Phase 11：CRC 标定与距离空间解耦](#12-phase-11crc-标定与距离空间解耦)
13. [Benchmark 体系](#13-benchmark-体系)
14. [构建与测试](#14-构建与测试)
15. [延迟优化项（UNDO.txt）](#15-延迟优化项)
16. [关键设计决策记录](#16-关键设计决策记录)
17. [接手开发指南](#17-接手开发指南)
18. [Phase 12：ExRaBitQ 对齐（多 bit 距离精度修复）](#18-phase-12exrabitq-对齐)
19. [Phase 13：SuperKMeans 集成](#19-phase-13superkmeans-集成)
20. [Phase 14：存储格式 v7 双区域布局](#20-phase-14存储格式-v7-双区域布局)
21. [Phase 15：FastScan + AVX-512](#21-phase-15fastscan--avx-512)
22. [待实施项](#22-待实施项)

---

## 1. 项目概述

VDB VectorRetrival 是一个面向高维向量近似最近邻搜索（ANNS）的存储与查询引擎，支持向量相似度搜索与原始数据（图片、音频、视频、文本）的关联检索。

**核心算法流水线**：
```
IVF 粗聚类 → Stage 1: FastScan VPSHUFB batch-32 (14-bit query × 1-bit codes)
           → ConANN 三分类 (SafeIn / SafeOut / Uncertain)
           → Stage 2: ExRaBitQ 精确估算 (4-bit codes, 仅 Uncertain)
           → Rerank 精排 → Payload 读取
```

**数据流水线**：
```
Build:  原始向量 + PayloadFn(id, raw_data)
          ↓
        IVF 聚类 → 按聚类写入 data.dat（行存：vec | payload_cols...）
          ↓
        RaBitQ 编码 → 写入 cluster.clu（codes + address blocks）
          ↓
        序列化 segment.meta（IvfParams + payload_schemas + ...）

Query:  query_vec → FindNearestClusters(nprobe)
          ↓
        io_uring/pread 异步读取 cluster blocks
          ↓
        RaBitQ 估算 → ConANN 三分类 → SafeIn/Uncertain 触发 rerank I/O
          ↓
        Early Stop 检查（TopK < d_k → 跳过剩余 cluster）
          ↓
        L2Sqr rerank → TopK collector → Payload 读取 → SearchResults
```

**技术栈**：C++17, AVX2 SIMD, FlatBuffers, GoogleTest, CMake, liburing

**设计特点**：
- 统一存储：单 `data.dat` + 单 `cluster.clu` per segment
- Address Column 通过 bit-packing + 多流 SIMD（K=8）解码实现记录定位
- RaBitQ 1-bit 随机旋转量化（XOR + popcount 快速距离估算）
- 页对齐存储（4KB 粒度），pread 随机读取
- ConANN 三分类：全局预计算 d_k，加性阈值 d_k ± 2ε
- 异步 I/O 查询管线：io_uring 滑动窗口预取 + 交错 probe/rerank
- Early Stop：TopK 距离低于 d_k 时提前终止聚类探测，减少不必要的 I/O 和计算
- Payload 管道：行存 payload 列（INT64/BYTES/STRING），支持多模态原始数据
- PayloadFn 回调模式：Build 时延迟生成 payload，零拷贝写入

---

## 2. 已完成模块一览

### 文件统计

| 阶段 | 头文件 | 实现文件 | 测试文件 | 测试用例数 |
|------|--------|---------|---------|-----------|
| Phase 0 Common | 5 | 1 | 4 | ~35 |
| Phase 1 Schema | 3 (.fbs) | 0 | 2 | ~20 |
| Phase 2 Columns | 3 | 3 | 2 | ~66 |
| Phase 3 SIMD+Codec | 7 | 7 | 7 | ~120 |
| Phase 4 RaBitQ | 3 | 3 | 4 | ~64 |
| Phase 5 Storage | 5 | 5 | 4 | ~80 |
| Phase 6 IVF Index | 3→5 | 3→4 | 4→6 | ~36→~50 |
| Phase 7 Query Pipeline | 7 | 6 | 7 | ~50 |
| Phase 8 Payload + IO | 2 | 2 | 3 | ~12 |
| Phase 9 Early Stop | 0 | 0 | 1 | ~3 |
| Phase 10 CRC | 2 | 1 | 2 | ~18 |
| Phase 11 Decouple CRC | 0 | 0 | 0 | — (重构) |
| **合计** | **~42** | **~34** | **~40** | **~530+** |

### 库依赖关系

```
vdb_io ── vdb_common (INTERFACE)

vdb_crc
  ├── vdb_simd
  ├── vdb_rabitq
  └── GSL (PRIVATE)

vdb_query
  ├── vdb_index
  │     ├── vdb_storage
  │     │     ├── vdb_columns
  │     │     ├── vdb_codec ── vdb_simd
  │     │     └── vdb_rabitq ── vdb_simd
  │     ├── vdb_crc (PRIVATE)
  │     ├── vdb_rabitq
  │     ├── vdb_simd
  │     └── vdb_schema ── flatbuffers
  ├── vdb_storage
  ├── vdb_simd
  ├── vdb_rabitq
  └── liburing (PRIVATE)

vdb_common (INTERFACE) ← 所有模块都依赖
```

---

## 3. Phase 0–2：基础设施

### 3.1 Common（Phase 0）

**文件**：
- `include/vdb/common/types.h` — 核心类型定义
- `include/vdb/common/status.h` — `Status` / `StatusOr<T>` 错误处理
- `include/vdb/common/distance.h` — 精确 L2 距离（非 SIMD，用于验证）
- `include/vdb/common/aligned_alloc.h` — 64 字节对齐内存分配（SIMD 用）
- `include/vdb/common/macros.h` — `VDB_FORCE_INLINE`, `VDB_LIKELY`, `VDB_USE_AVX2` 等编译器宏

**types.h 中定义的关键类型**：

```cpp
// 基本标识
using VecID     = uint64_t;    // 向量全局唯一标识
using RowID     = uint64_t;    // 行号
using SegmentID = uint32_t;    // Segment 标识
using ListID    = uint32_t;    // 倒排列表 ID
using Dim       = uint32_t;    // 向量维度
using ColumnID  = uint32_t;    // 列标识
using ClusterID = uint32_t;    // 聚类 ID
using FileID    = uint16_t;    // DataFile 标识

// 枚举
enum class MetricType : uint8_t { L2, IP, COSINE };
enum class DType : uint8_t { FLOAT32, INT32, INT64=3, STRING=20, BYTES=21, ... };
enum class ResultClass : uint8_t { SafeIn, SafeOut, Uncertain };

// Datum — 类型擦除的列值
struct Datum {
    DType dtype;
    union { float f32; int32_t i32; int64_t i64; } fixed;
    std::string var_data;  // STRING / BYTES
    static Datum Int64(int64_t v);
    static Datum Bytes(std::string data);
    static Datum String(std::string data);
};

// ColumnSchema — 列描述
struct ColumnSchema {
    uint32_t id;
    std::string name;
    DType dtype;
    bool nullable;
};

// 存储地址
struct AddressEntry {
    uint64_t offset;  // DataFile 中的字节偏移
    uint32_t size;    // 记录字节长度
};

// RaBitQ 配置
struct RaBitQConfig {
    uint8_t  bits       = 1;      // 量化位数
    uint32_t block_size = 64;     // 地址块粒度
    float    c_factor   = 5.75f;  // ConANN epsilon 系数
};
```

**Status 系统**：
```cpp
Status s = DoSomething();
if (!s.ok()) return s;

StatusOr<ClusterStoreReader> reader = ClusterStoreReader::Open(path);
if (!reader.ok()) return reader.status();
auto& r = reader.value();
```

支持的错误码：`OK`, `InvalidArgument`, `NotFound`, `IOError`, `Corruption`, `AlreadyExists`, `NotSupported`, `OutOfMemory`, `Internal`, `Cancelled`, `Timeout`

### 3.2 Schema（Phase 1）

**FlatBuffers Schema 文件**：

`schema/columns.fbs`：
- `DataType` 枚举（FLOAT32, INT32, INT64, STRING, BYTES, ...）
- `EncodingType` 枚举（NONE, DICT, BITPACK, DELTA, RLE, FOR, RABITQ）
- `ColumnChunkMeta` table（column_id, encoding, num_values, compressed_size, ...）

`schema/segment_meta.fbs`：
```fbs
table PayloadColumnSchema {    // Phase 8 新增
    id: uint32;
    name: string;
    dtype: uint8;              // 映射 C++ DType 枚举值
    nullable: bool;
}

table IvfParams {
    nlist: uint32;
    nprobe: uint32;
    balance_factor: float;
    centroids_offset: uint64;
    centroids_length: uint64;
}

table ConANNParams {
    epsilon: float;                  // ε_ip: 内积估计误差上界 P95
    d_k: float;                      // 全局 top-k 参考距离 (L2²)
    calibration_samples: uint32;
    calibration_topk: uint32;
    calibration_percentile: float;
}

table RaBitQParams {
    bits: uint8;
    block_size: uint32;
    c_factor: float;
}

table CrcParams {                        // Phase 11 新增
    scores_file: string;               // 相对路径: "crc_scores.bin"
    num_calibration_queries: uint32;
    calibration_top_k: uint32;
}

table SegmentMeta {
    segment_id: uint32;
    dimension: uint32;
    metric_type: MetricType;
    ivf_params: IvfParams;
    conann_params: ConANNParams;
    crc_params: CrcParams;               // Phase 11 新增
    rabitq_params: RaBitQParams;
    clusters: [ClusterMeta];
    payload_schemas: [PayloadColumnSchema];  // Phase 8 新增
    // ... 其他字段
}
```

`schema/record.fbs`：
- `DataFileHeader` table（magic, version, cluster_id, num_records, dimension, column_schemas, ...）
- `PayloadMode` 枚举（INLINE vs EXTERN），4KB 阈值分界
- `ExternRef`（file_id, offset, length）用于大 blob 引用

**代码生成**：CMake `flatc --cpp --scoped-enums` → 生成 `_generated.h` 到 `${CMAKE_BINARY_DIR}/generated/`

### 3.3 ColumnStore（Phase 2）

**文件**：
- `include/vdb/columns/column_chunk_writer.h` → 单列写入
- `include/vdb/columns/column_chunk_reader.h` → 单列读取
- `include/vdb/columns/column_store.h` → 多列管理器

**功能**：支持固定长度列（INT32, FLOAT, INT64）和变长列（STRING, BYTES）的分块读写。使用 offset-based 寻址（ColumnLocator），支持投影下推（Projection Pushdown）。被 Phase 5 的 DataFileWriter/Reader 复用。

---

## 4. Phase 3：SIMD + Codec

### 4.1 L2 距离（distance_l2）

**文件**：`include/vdb/simd/distance_l2.h` + `src/simd/distance_l2.cpp`

```cpp
namespace vdb::simd {
    float L2Sqr(const float* a, const float* b, Dim dim);
}
```

- AVX2 路径：每次处理 8 个 float，`_mm256_sub_ps` → `_mm256_fmadd_ps` 累加
- 尾部处理：`dim % 8` 个元素用标量处理
- Horizontal sum：`_mm256_hadd_ps` → `_mm_add_ps` → `_mm_cvtss_f32`
- 使用 `loadu`（非对齐加载），支持任意 dim

### 4.2 Bit Unpack（bit_unpack）

**文件**：`include/vdb/simd/bit_unpack.h` + `src/simd/bit_unpack.cpp`

```cpp
namespace vdb::simd {
    void BitUnpack(const uint8_t* packed, uint8_t bit_width,
                   uint32_t* out, uint32_t count);
    void BitUnpack1Bit_AVX2(const uint8_t* packed, uint32_t* out, uint32_t count);
    void BitUnpackScalar(const uint8_t* packed, uint8_t bit_width,
                         uint32_t* out, uint32_t count);
}
```

- 1-bit AVX2 特化：per-byte broadcast → `_mm256_srlv_epi32` lane shifts → mask → 8 值/字节
- 标量路径：支持任意位宽（1–32），跨字节边界正确处理
- LSB 优先约定：第 i 位 = packed[i/8] 的第 `i%8` 位

### 4.3 Prefix Sum（prefix_sum）

**文件**：`include/vdb/simd/prefix_sum.h` + `src/simd/prefix_sum.cpp`

```cpp
namespace vdb::simd {
    void ExclusivePrefixSum(const uint32_t* in, uint32_t* out, uint32_t count);
    void ExclusivePrefixSumMulti(const uint32_t* interleaved_in,
                                  uint32_t* interleaved_out,
                                  uint32_t count, uint32_t num_streams);
}
```

- 单流：out[i] = sum(in[0..i-1])
- 多流（AVX2）：交错布局 `buf[j*8+k] = stream_k[j]`，每条 SIMD lane 运行一个独立流
- 无跨 lane 依赖，每步只需一条 `_mm256_add_epi32`

### 4.4 Transpose（transpose）

**文件**：`include/vdb/simd/transpose.h` + `src/simd/transpose.cpp`

```cpp
namespace vdb::simd {
    void Transpose8xN(const uint32_t* const* streams_in,
                       uint32_t* interleaved_out,
                       uint32_t num_streams, uint32_t count_per_stream);
    void TransposeNx8(const uint32_t* interleaved_in,
                       uint32_t* const* streams_out,
                       uint32_t num_streams, uint32_t count_per_stream);
}
```

- SoA ↔ 交错布局转换，支持 K=1..8 流
- AVX2 8×8 子块转置：`_mm256_unpacklo/hi_epi32/epi64` + `_mm256_permute2x128_si256`
- 不足 8 流时自动零填充

### 4.5 Popcount（popcount）

**文件**：`include/vdb/simd/popcount.h` + `src/simd/popcount.cpp`

```cpp
namespace vdb::simd {
    uint32_t Popcount64(uint64_t x);
    uint32_t PopcountXor(const uint64_t* a, const uint64_t* b, uint32_t num_words);
    uint32_t PopcountTotal(const uint64_t* code, uint32_t num_words);
}
```

- `PopcountXor`：RaBitQ 快速路径核心——计算 Hamming 距离
- AVX2：VPSHUFB lookup-table popcount（Harley-Seal 方法），4 uint64_t/迭代
- Scalar：`__builtin_popcountll` 回退

### 4.6 BitpackCodec + DictCodec

**BitpackCodec**（`codec/bitpack_codec.h`）：
- `ComputeMinBitWidth(values, count, max_packable_value)` — 计算最小位宽
- `Encode(values, count, bit_width)` — 标量打包（离线路径）
- `Decode(packed, bit_width, count)` — SIMD 解包（热路径，委托 simd::BitUnpack）

**DictCodec**（`codec/dict_codec.h`）：
- `BuildDict(values)` — 构建字典（插入序保留）
- `Decode(indices, dict)` — 从索引重建字符串

---

## 5. Phase 4：RaBitQ 量化

### 5.1 算法概述

RaBitQ 的算法流程（**不是 PQ 的 min/max 标量量化**）：

```
原始向量 → 中心化(减 centroid) → 归一化 → 随机正交旋转 P^T → 符号量化 → bit-pack
```

**距离估算公式**：

$$\|o - q\|^2 \approx \|o-c\|^2 + \|q-c\|^2 - 2\|o-c\|\cdot\|q-c\|\cdot\langle\bar{q}, \hat{o}\rangle$$

其中内积通过 XOR + popcount 快速计算：

$$\langle\bar{q}, \hat{o}\rangle \approx \frac{2}{L}\text{sum\_x} - \frac{2}{\sqrt{L}}(sum\_x - 2H) - \frac{1}{\sqrt{L}}\text{sum\_q}$$

$H$ 为查询符号码和数据库符号码的 Hamming 距离。

### 5.2 旋转矩阵模块

**文件**：`include/vdb/rabitq/rabitq_rotation.h` + `src/rabitq/rabitq_rotation.cpp`

```cpp
class RotationMatrix {
public:
    explicit RotationMatrix(Dim dim);
    void GenerateRandom(uint64_t seed);       // Gram-Schmidt QR, O(L²)
    bool GenerateHadamard(uint64_t seed, bool use_srht = true);  // Walsh-Hadamard × 随机对角, O(L log L)
    void Apply(const float* in, float* out) const;      // out = P^T × in（编码方向）
    void ApplyInverse(const float* in, float* out) const; // out = P × in（重建方向）
    Status Save(const std::string& path) const;
    static StatusOr<RotationMatrix> Load(const std::string& path, Dim dim);
};
```

- Move-only 类型，无拷贝
- Hadamard 路径支持快速变换（O(L log L) vs O(L²)）

### 5.3 RaBitQ 编码器

**文件**：`include/vdb/rabitq/rabitq_encoder.h` + `src/rabitq/rabitq_encoder.cpp`

```cpp
struct RaBitQCode {
    std::vector<uint64_t> code;  // bit-packed 符号码，ceil(L/64) 个 uint64_t
    float norm_sq;               // ‖o - c‖²
    uint32_t sum_x;              // popcount(code)
};

class RaBitQEncoder {
public:
    RaBitQEncoder(Dim dim, const RotationMatrix& P);
    RaBitQCode Encode(const float* vec, const float* centroid) const;
    std::vector<RaBitQCode> EncodeBatch(const float* vecs, uint32_t n,
                                         const float* centroid) const;
};
```

**编码流程**：
```
vec ─┬─ r = vec - centroid
     ├─ norm_sq = ‖r‖²
     ├─ r̄ = r / ‖r‖     (归一化)
     ├─ r̄' = P^T · r̄    (旋转)
     └─ code[i] = (r̄'[i] >= 0) ? 1 : 0  (符号量化)
        → bit-pack 到 uint64_t 数组
```

### 5.4 RaBitQ 距离估算器

**文件**：`include/vdb/rabitq/rabitq_estimator.h` + `src/rabitq/rabitq_estimator.cpp`

```cpp
struct PreparedQuery {
    std::vector<float>    rotated;     // q' = P^T × q̄
    std::vector<uint64_t> sign_code;   // q' 的符号码
    float                 norm_qc;     // ‖q - c‖₂
    float                 norm_qc_sq;  // ‖q - c‖₂²
    float                 sum_q;       // Σ q'[i]
};

class RaBitQEstimator {
public:
    PreparedQuery PrepareQuery(const float* query, const float* centroid,
                                const RotationMatrix& P) const;
    float EstimateDistance(const PreparedQuery& pq, const RaBitQCode& code) const;
    float EstimateDistanceAccurate(const PreparedQuery& pq, const RaBitQCode& code) const;
    void EstimateDistanceBatch(const PreparedQuery& pq, const RaBitQCode* codes,
                                uint32_t count, float* distances) const;
};
```

**快速路径**（热循环内核，O(L/64)）：
```
hamming = PopcountXor(pq.sign_code, db_code.code)
ip_est = (2/√L)(sum_x - 2·hamming) - (1/√L)·sum_q
dist² ≈ norm_oc² + norm_qc² - 2·norm_oc·norm_qc·ip_est
```

| 路径 | 复杂度 | 精度 | 用途 |
|------|--------|------|------|
| 快速（popcount） | O(L/64) | 1-bit 量化 | 热循环 top-K 初选 |
| 精确（逐位浮点） | O(L) | 较高 | 精排验证 |

---

## 6. Phase 5：Storage 层

### 6.1 AddressColumn

**文件**：`include/vdb/storage/address_column.h` + `src/storage/address_column.cpp`

```cpp
class AddressColumn {
public:
    static constexpr uint32_t kMultiStreamWidth = 8;  // 多流 SIMD 宽度

    static EncodedAddressColumn Encode(
        const std::vector<AddressEntry>& entries,
        uint32_t fixed_packed_size = 64,
        uint32_t page_size = 4096);

    static void DecodeSingleBlock(const AddressColumnLayout& layout,
                                   const AddressBlock& block,
                                   uint32_t record_count,
                                   std::vector<AddressEntry>& out);

    static void DecodeMultiStream(const AddressColumnLayout& layout,
                                   const AddressBlock* blocks,
                                   uint32_t num_blocks,
                                   AddressEntry* out_entries);

    static Status DecodeBatchBlocks(const AddressColumnLayout& layout,
                                     const std::vector<AddressBlock>& blocks,
                                     uint32_t total_records,
                                     std::vector<AddressEntry>& out);
};
```

**页对齐存储设计**：
- `page_size=4096` 时，offset/size 除以 page_size 后存储为页索引/页数
- `base_offset` 类型为 `uint32_t`（页索引），可寻址 16TB@4KB 页大小
- 解码时乘回 page_size，最终 `AddressEntry::offset` 为 `uint64_t`

**多流 SIMD 解码管线**（DecodeMultiStream）：
```
Phase 1: BitUnpack 每个块 → K 个 SoA 缓冲区
Phase 2: Transpose8xN → 交错布局（同一元素索引跨流连续）
Phase 3: ExclusivePrefixSumMulti → 交错前缀和（每条 lane 独立累加）
Phase 4: TransposeNx8 → 反转置回 SoA
Phase 5: Materialize → uint32 页索引展宽为 uint64 字节偏移
```

### 6.2 DataFileWriter / DataFileReader

**文件**：
- `include/vdb/storage/data_file_writer.h` + `src/storage/data_file_writer.cpp`
- `include/vdb/storage/data_file_reader.h` + `src/storage/data_file_reader.cpp`

```cpp
class DataFileWriter {
public:
    Status Open(const std::string& path, uint32_t cluster_id, Dim dim,
                const std::vector<ColumnSchema>& payload_schemas,
                uint32_t page_size = kDefaultPageSize);
    Status WriteRecord(const float* vec, const std::vector<Datum>& payload,
                       AddressEntry& out_entry);
    Status Finalize();
};

class DataFileReader {
public:
    Status Open(const std::string& path, Dim dim,
                const std::vector<ColumnSchema>& payload_schemas);
    Status ReadRecord(const AddressEntry& addr, std::vector<float>* vec,
                      std::vector<Datum>* payload);
    Status ReadVector(const AddressEntry& addr, std::vector<float>* vec);
    Status ReadRaw(uint64_t offset, uint32_t length, uint8_t* buffer);
    Status ParsePayload(const uint8_t* buf, uint32_t buf_len,
                        uint32_t offset, std::vector<Datum>* payload);
};
```

**记录格式**（行存 row-major）：
```
┌────────────────────────────────────────┐
│ raw_vector: D × float32               │
│ fixed_payload_col_0: (e.g. int64)     │
│ fixed_payload_col_1: ...              │
│ var_len_offsets + var_len_data         │
│ [padding to page_size alignment]      │
└────────────────────────────────────────┘
```

- 使用 `pread` 系统调用（线程安全，不依赖文件指针）
- 固定长度列（INT64 等）直接按序排列
- 变长列（BYTES, STRING）用 offset 表 + 连续数据块

### 6.3 ClusterStore

**文件**：`include/vdb/storage/cluster_store.h` + `src/storage/cluster_store.cpp`

```cpp
class ClusterStoreWriter {
public:
    Status Open(const std::string& path, uint32_t num_clusters, Dim dim,
                const RaBitQConfig& rabitq_config);
    Status BeginCluster(uint32_t cluster_id, uint32_t num_records, const float* centroid);
    Status WriteVectors(const std::vector<RaBitQCode>& codes);
    Status WriteAddressBlocks(const std::vector<AddressBlock>& blocks);
    Status EndCluster();
    Status Finalize(const std::string& data_file_path);
};

class ClusterStoreReader {
public:
    Status Open(const std::string& path);
    uint32_t GetNumRecords(uint32_t cluster_id) const;
    const float* GetCentroid(uint32_t cluster_id) const;
    Status EnsureClusterLoaded(uint32_t cluster_id);  // 延迟加载
    AddressEntry GetAddress(uint32_t cluster_id, uint32_t local_idx) const;
    const uint64_t* GetCodePtr(uint32_t cluster_id, uint32_t local_idx) const;
    Status ParseClusterBlock(const uint8_t* buf, uint32_t len, uint32_t cluster_id);
};
```

**.clu 文件格式（v4）**：
```
Global Header:
  magic(u32) + version(u32=4) + num_clusters(u32) + dim(u32)
  + rabitq config + data_file_path

Lookup Table (in-memory):
  per cluster: cluster_id(u32) + num_records(u32) + centroid(dim×f32)
               + block_offset(u64) + block_size(u64)

Cluster Blocks (per cluster, on-disk):
  RaBitQ codes: N × ceil(dim/64) × 8B
  Address packed data: block0.packed ... blockM-1.packed
  Mini-Trailer:
    page_size(u32) + bit_width(u8) + block_granularity(u32)
    + fixed_packed_size(u32) + last_packed_size(u32) + num_address_blocks(u32)
    + base_offsets[M]: u32[M]
    + mini_trailer_size(u32) + block_magic(u32)
```

### 6.4 Segment

**文件**：`include/vdb/storage/segment.h` + `src/storage/segment.cpp`

```cpp
class Segment {
public:
    Status Open(const std::string& dir, const std::vector<ColumnSchema>& payload_schemas);

    // 转发 ClusterStoreReader
    uint32_t num_clusters() const;
    uint32_t GetNumRecords(uint32_t cluster_id) const;
    const float* GetCentroid(uint32_t cluster_id) const;
    Status EnsureClusterLoaded(uint32_t cluster_id);
    AddressEntry GetAddress(uint32_t cluster_id, uint32_t local_idx) const;

    // 转发 DataFileReader
    Status ReadVector(const AddressEntry& addr, std::vector<float>* vec);
    Status ReadRecord(const AddressEntry& addr, std::vector<float>* vec,
                      std::vector<Datum>* payload);

    // 异步 I/O 支持
    int clu_fd() const;  // cluster.clu 文件描述符
    int dat_fd() const;  // data.dat 文件描述符
};
```

Segment 是 ClusterStoreReader + DataFileReader 的薄封装，对外暴露统一的集群 + 记录访问接口，同时暴露 fd 给 io_uring 使用。

---

## 7. Phase 6：IVF 索引

### 7.1 ConANN 三分类

**文件**：`include/vdb/index/conann.h` + `src/index/conann.cpp`

```cpp
class ConANN {
    float epsilon_;    // ε = c·2^(-B/2)/√D
    float d_k_;        // 全局参考距离
    float tau_in_;     // = d_k - 2ε
    float tau_out_;    // = d_k + 2ε

public:
    ConANN(float epsilon, float d_k);
    static ConANN FromConfig(const RaBitQConfig& cfg, Dim dim, float d_k);
    ResultClass Classify(float approx_dist) const;
    static float CalibrateDistanceThreshold(
        const float* vectors, uint32_t N, Dim dim,
        uint32_t num_samples, uint32_t top_k,
        float percentile, uint64_t seed);
};
```

**三分类规则**：
- SafeIn：`approx_dist < d_k - 2ε` → 一定在 top-K 中，直接进入结果
- SafeOut：`approx_dist > d_k + 2ε` → 一定不在 top-K 中，跳过
- Uncertain：需要精确 L2 rerank 确认

**CalibrateDistanceThreshold**：
1. 随机抽 `num_samples` 个向量作查询
2. 暴力计算每个查询到所有向量的 L2 距离，取第 top_k 距离
3. 对所有样本的 top-k 距离排序，取 percentile 分位数作为 d_k

### 7.2 IvfIndex

**文件**：`include/vdb/index/ivf_index.h` + `src/index/ivf_index.cpp`

```cpp
class IvfIndex {
    Dim dim_;
    uint32_t nlist_;
    std::vector<float> centroids_;  // nlist × dim 行主序
    ConANN conann_;
    Segment segment_;
    std::vector<ColumnSchema> payload_schemas_;

public:
    Status Open(const std::string& dir);
    std::vector<ClusterID> FindNearestClusters(const float* query, uint32_t nprobe) const;
    Dim dim() const;
    uint32_t nlist() const;
    const std::vector<ColumnSchema>& payload_schemas() const;
};
```

**Open 流程**：
1. 读 `segment.meta`（FlatBuffers 反序列化）
2. 解析 IvfParams → nlist, nprobe
3. 解析 ConANNParams → epsilon, d_k → 构造 ConANN
4. 加载 `centroids.bin`（裸 float 数组）
5. 加载 `rotation.bin`（RotationMatrix）
6. **解析 payload_schemas**（Phase 8 新增）→ 填充 payload_schemas_
7. `segment_.Open(dir, payload_schemas_)` → 打开 cluster.clu + data.dat

### 7.3 IvfBuilder

**文件**：`include/vdb/index/ivf_builder.h` + `src/index/ivf_builder.cpp`

```cpp
struct IvfBuilderConfig {
    uint32_t nlist = 16;
    uint32_t max_iterations = 20;
    float tolerance = 1e-4f;
    uint64_t seed = 42;
    float balance_factor = 0.0f;     // 0=标准K-means, >0=容量受限
    RaBitQConfig rabitq = {};
    std::vector<ColumnSchema> payload_schemas = {};  // Payload 列定义
    uint32_t calibration_samples = 100;
    uint32_t calibration_topk = 10;
    float calibration_percentile = 0.99f;
    uint32_t page_size = 1;
    uint64_t segment_id = 0;
    uint32_t nprobe = 1;
    uint32_t epsilon_samples = 20;       // ε_ip 标定采样数
    float epsilon_percentile = 0.95f;    // ε_ip 标定分位数
    const float* calibration_queries = nullptr;  // 跨模态查询向量
    uint32_t num_calibration_queries = 0;
    uint32_t crc_top_k = 0;             // CRC 分数预计算 (0=跳过)
};

using PayloadFn = std::function<std::vector<Datum>(uint32_t vec_index)>;

class IvfBuilder {
public:
    explicit IvfBuilder(const IvfBuilderConfig& config);
    Status Build(const float* vectors, uint32_t N, Dim dim,
                 const std::string& output_dir,
                 PayloadFn payload_fn = nullptr);
    const std::vector<uint32_t>& assignments() const;
    const std::vector<float>& centroids() const;
    float calibrated_dk() const;
    void SetProgressCallback(std::function<void(uint32_t, uint32_t)> cb);
};
```

**Build 流程**（5 个 Phase）：

**Phase A — 容量受限 K-means++**：
- K-means++ 初始化（距离概率加权选择初始质心）
- 迭代 Assign + Update：
  - `balance_factor=0`：标准最近质心分配
  - `balance_factor>0`：超容量集群的最远向量迁移到最近的未满集群
- 收敛判定：max centroid shift < tolerance

**Phase B — ConANN d_k / ε_ip 校准**：
- 调用 `CalibrateDistanceThreshold`，返回全局 d_k
- 校准 ε_ip（内积估计误差上界），支持跨模态查询向量

**Phase C — 逐 cluster 写入**：
- 统一 DataFileWriter 写入 `data.dat`：每条记录 = vec + `payload_fn(idx)` 的返回值
- RaBitQEncoder 编码向量 → ClusterStoreWriter 写入 `cluster.clu`

**Phase D — 写 segment.meta**：
- FlatBuffers `SegmentMetaBuilder` 序列化所有元数据
- 包含 payload_schemas、CrcParams

**Phase E — CRC 分数预计算**（可选，`crc_top_k > 0`）：
- 构建 ClusterData（含 RaBitQ codes）
- 全探测标定查询 → ComputeScoresRaBitQ → WriteScores("crc_scores.bin")
- CrcParams 写入 segment.meta

### 7.4 Benchmark

**文件**：`tests/index/bench_ivf_quality.cpp`

独立可执行文件，评估 RaBitQ 精度（Spearman 相关性、相对误差）和 ConANN 三分类分布（SafeIn / SafeOut / Uncertain 比例）。

---

## 8. Phase 7：查询流水线

### 8.1 架构概述

单线程异步 I/O 查询管线，两阶段交替执行：

```
┌─────────────────────────────────────────────────────────────────┐
│ Phase 1: PrefetchClusters（滑动窗口 io_uring 批量提交）          │
│                                                                 │
│ Phase 2: ProbeAndDrainInterleaved                              │
│   ┌──────────────────────────────────────────────┐              │
│   │ for each cluster:                            │              │
│   │   CQE drain → DispatchCompletion             │              │
│   │   ├─ CLUSTER_BLOCK → ParseClusterBlock       │              │
│   │   ├─ VEC_ONLY/VEC_ALL → RerankConsumer       │              │
│   │   └─ PAYLOAD → RerankConsumer.CachePayload   │              │
│   │                                              │              │
│   │   ProbeCluster:                              │              │
│   │     RaBitQ EstimateBatch → ConANN Classify   │              │
│   │     SafeIn → submit VEC_ALL or VEC_ONLY      │              │
│   │     Uncertain → submit VEC_ONLY              │              │
│   │     SafeOut → skip                           │              │
│   │                                              │              │
│   │   ★ Early Stop 检查:                         │              │
│   │     if Full() && TopDistance() < d_k → break │              │
│   └──────────────────────────────────────────────┘              │
│                                                                 │
│ Phase 3: FinalDrain（消耗剩余 CQE）                             │
│                                                                 │
│ Phase 4: FetchMissingPayloads（为 TopK 中缺失 payload 的发读请求）│
│                                                                 │
│ Phase 5: AssembleResults（收集 TopK → SearchResults）            │
└─────────────────────────────────────────────────────────────────┘
```

### 8.2 AsyncReader（异步 I/O 接口）

**文件**：`include/vdb/query/async_reader.h`

```cpp
class AsyncReader {
public:
    virtual void PrepRead(int fd, uint8_t* buf, uint32_t len, uint64_t offset) = 0;
    virtual void Submit() = 0;
    virtual uint32_t Poll(uint8_t** out, uint32_t max_count) = 0;
    virtual uint32_t WaitAndPoll(uint8_t** out, uint32_t max_count) = 0;
    virtual uint32_t InFlight() const = 0;
};
```

**两种实现**：

| 实现 | 机制 | 适用场景 |
|------|------|----------|
| `IoUringReader` | `io_uring` SQE/CQE | 生产环境，Linux ≥ 5.1 |
| `PreadFallbackReader` | `pread()` 同步模拟 | 测试、无 io_uring 环境 |

- `IoUringReader`：pimpl 封装 `io_uring_ring`，`user_data` 指向 buffer 地址
- `PreadFallbackReader`：`PrepRead` 存入 pending[]，`Submit` 批量 pread，`WaitAndPoll` 返回全部

### 8.3 SearchConfig + SearchContext

**文件**：`include/vdb/query/search_context.h`

```cpp
struct SearchConfig {
    uint32_t top_k = 10;
    uint32_t nprobe = 4;
    uint32_t probe_batch_size = 64;
    uint32_t io_queue_depth = 256;
    uint32_t safein_all_threshold = 256 * 1024;  // 256KB
    uint32_t prefetch_depth = 8;
    uint32_t refill_threshold = 2;
    uint32_t refill_count = 4;
    bool early_stop = true;  // Phase 9: TopK < d_k 时提前终止探测
};
```

**`safein_all_threshold`**：SafeIn 分类的记录，如果 `record_size < threshold`，读取 VEC_ALL（向量+payload 一次读取）；否则只读 VEC_ONLY（仅向量），payload 稍后按需读取。

### 8.4 OverlapScheduler

**文件**：`include/vdb/query/overlap_scheduler.h` + `src/query/overlap_scheduler.cpp`

```cpp
class OverlapScheduler {
public:
    OverlapScheduler(IvfIndex& index, AsyncReader& reader, const SearchConfig& config);
    SearchResults Search(const float* query_vec);
};
```

Search 返回 `SearchResults`（RAII 包装的 `vector<SearchResult>` + `SearchStats`），每个 `SearchResult` 包含 `{float distance, vector<Datum> payload}`。通过 `results.stats()` 可访问查询统计（包括 early_stopped、clusters_skipped 等）。

### 8.5 RerankConsumer

**文件**：`include/vdb/query/rerank_consumer.h` + `src/query/rerank_consumer.cpp`

```cpp
class RerankConsumer {
public:
    void ConsumeVec(const uint8_t* buf, const AddressEntry& addr);   // VEC_ONLY
    void ConsumeAll(const uint8_t* buf, const AddressEntry& addr);   // VEC_ALL
    void ConsumePayload(const uint8_t* buf, const AddressEntry& addr); // PAYLOAD
    bool HasPayload(uint64_t offset) const;
    std::string TakePayload(uint64_t offset);
};
```

三种消费模式：
- **VEC_ONLY**：buffer 中只有向量 → L2Sqr rerank → TryInsert 到 TopK
- **VEC_ALL**：buffer 中有向量+payload → L2Sqr rerank → TryInsert → 如果进入 TopK 则缓存 payload
- **PAYLOAD**：只有 payload → 存入缓存，稍后组装到结果

Payload 缓存以 `offset` 为 key（每条记录的唯一标识），TopK 确定后清理未使用的缓存条目。

### 8.6 其他组件

| 组件 | 文件 | 功能 |
|------|------|------|
| ResultCollector | `query/result_collector.h` | TopK max-heap，`TryInsert(distance, addr)` |
| BufferPool | `query/buffer_pool.h` | 单线程 buffer 池，`Acquire(size)` / `Release(buf)` |
| ParsedCluster | `query/parsed_cluster.h` | 已解析的 cluster 缓存（codes + addresses） |
| SearchResults | `query/search_results.h` | `vector<SearchResult>` + `SearchStats` 的 RAII 包装 |

---

## 9. Phase 8：Payload 数据管道 + IO 加载器

### 9.1 问题背景

Phase 7 之前的代码存在 3 个断点导致 payload 数据无法流通：

1. **断点 1**（`ivf_builder.cpp:321`）：`WriteRecord(vec, {}, entry)` 始终传空 payload
2. **断点 2**（`segment_meta.fbs`）：无 `PayloadColumnSchema` 表，payload_schemas 无法持久化
3. **断点 3**（`ivf_index.cpp:112`）：`payload_schemas_` 始终为空，`segment_.Open()` 拿不到 schema

查询侧代码（OverlapScheduler、RerankConsumer）已完整支持 payload 读取，仅构建侧和元数据持久化需要修复。

### 9.2 修复内容

**FlatBuffers Schema**：新增 `PayloadColumnSchema` 表和 `SegmentMeta.payload_schemas` 字段。

**IvfBuilder**：
- 新增 `PayloadFn` 回调类型（`std::function<std::vector<Datum>(uint32_t vec_index)>`）
- `Build()` 接受可选的 `PayloadFn`，默认 nullptr = 无 payload（向后兼容）
- `WriteRecord` 调用 `payload_fn(idx)` 获取 payload
- segment.meta 序列化使用 `SegmentMetaBuilder` 模式（替代位置参数），包含 payload_schemas

**IvfIndex::Open**：从 FlatBuffers 反序列化 `payload_schemas`，填充 `payload_schemas_` 成员，传递给 `segment_.Open()`。

### 9.3 Payload 约定

标准两列 payload：

| 列 ID | 名称 | DType | 用途 |
|--------|------|-------|------|
| 0 | `id` | INT64 | 原始向量 ID，用于 recall 验证 |
| 1 | `data` | BYTES 或 STRING | 原始数据（图片二进制 / 文本 / 音频 / 视频） |

使用示例：
```cpp
IvfBuilderConfig cfg;
cfg.payload_schemas = {
    {0, "id",   DType::INT64, false},
    {1, "data", DType::BYTES, false},
};

PayloadFn payload_fn = [&](uint32_t vec_index) -> std::vector<Datum> {
    return {Datum::Int64(image_ids[vec_index]),
            Datum::Bytes(load_image_bytes(vec_index))};
};

builder.Build(vectors, N, dim, output_dir, payload_fn);
```

### 9.4 NpyReader

**文件**：`include/vdb/io/npy_reader.h` + `src/io/npy_reader.cpp`

```cpp
namespace vdb::io {
    struct NpyArrayFloat { std::vector<float> data; uint32_t rows; uint32_t cols; };
    struct NpyArrayInt64 { std::vector<int64_t> data; uint32_t count; };

    StatusOr<NpyArrayFloat> LoadNpyFloat32(const std::string& path);
    StatusOr<NpyArrayInt64> LoadNpyInt64(const std::string& path);
}
```

**npy 文件解析**：
1. 6 字节 magic `\x93NUMPY`
2. 2 字节版本号（v1: 2 字节 header_len, v2: 4 字节 header_len）
3. Python dict 字符串解析（`descr`, `fortran_order`, `shape`）
4. 定位 data_offset → 直接 read 原始数据

支持 v1.0 和 v2.0 格式。不支持大端和 Fortran order。

### 9.5 JsonlReader

**文件**：`include/vdb/io/jsonl_reader.h` + `src/io/jsonl_reader.cpp`

```cpp
namespace vdb::io {
    Status ReadJsonlLines(const std::string& path,
                          std::function<void(uint32_t, std::string_view)> fn);
}
```

逐行 `getline` 读取，跳过空行和纯空白行。不做 JSON 解析，原始行内容通过回调传递。

### 9.6 测试覆盖

| 测试 | 文件 | 用例 |
|------|------|------|
| Payload Roundtrip (BYTES) | `tests/index/payload_pipeline_test.cpp` | Build(PayloadFn) → Search → 验证 id + data |
| Payload Roundtrip (STRING) | 同上 | STRING 类型 payload 端到端验证 |
| Backward Compat | 同上 | Build(nullptr) → payload 为空 |
| Schema Roundtrip | 同上 | payload_schemas 持久化→恢复一致性 |
| NpyFloat32 | `tests/io/npy_reader_test.cpp` | coco_1k/image_embeddings.npy → (1000, 512) |
| NpyInt64 | 同上 | coco_1k/image_ids.npy → 1000, 首 id=139 |
| Jsonl | `tests/io/jsonl_reader_test.cpp` | coco_1k/metadata.jsonl → 1000 行 |

---

## 10. Phase 9：Early Stop 聚类探测提前终止

### 10.1 设计动机

标准 IVF 查询固定探测 `nprobe` 个聚类，不考虑已收集结果的质量。当 TopK 堆已被高质量结果填满时，继续探测剩余聚类是浪费 I/O 和 CPU。Early Stop 利用 ConANN 构建时校准的全局参考距离 `d_k`，在探测过程中判断是否可以提前终止。

### 10.2 Early Stop 条件

在每个聚类探测完成（ProbeCluster + Submit）后检查：

```cpp
if (config_.early_stop &&
    ctx.collector().Full() &&
    ctx.collector().TopDistance() < index_.conann().d_k()) {
    ctx.stats().early_stopped = true;
    ctx.stats().clusters_skipped = sorted_clusters.size() - 1 - i;
    break;
}
```

三个条件的含义：
- `config.early_stop`：用户可配置开关（默认 `true`）
- `collector.Full()`：TopK 堆已填满至少 `top_k` 个结果
- `collector.TopDistance() < conann.d_k()`：堆中最差距离已低于全局校准阈值

### 10.3 检查时机与 I/O 模型关系

Early Stop 检查位于 `ProbeAndDrainInterleaved` 循环中，**在 ProbeCluster + Submit 之后**。TopK 堆的状态取决于 WaitAndPoll 在等待聚类 block 时自然消费的 vec 完成事件：

```
for each cluster i:
    ①  WaitAndPoll 等待 cluster block → 自然消费 vec CQE → TopK 更新
    ②  ProbeCluster → 产生新的 vec 读请求
    ③  Submit 提交新请求
    ④  ★ Early Stop 检查 ★
    ⑤  滑动窗口补充
```

**关键设计决策**：不主动 drain（不调用 Poll() 消费所有待处理完成事件），TopK 状态是机会性的——取决于步骤 ① 中 WaitAndPoll 在等待聚类 block 时顺带消费了多少 vec 完成事件。

**I/O 后端行为差异**：

| 后端 | 行为 | Early Stop 效果 |
|------|------|----------------|
| `IoUringReader` | WaitAndPoll 等待聚类 block 时自然收到 vec CQE → TopK 持续更新 | ✅ 正常触发 |
| `PreadFallbackReader` | Submit 同步执行所有 pread，聚类 block 全部立即就绪 → WaitAndPoll 可能被跳过 | ⚠️ 可能不触发（预期行为） |

PreadFallback 是测试 fallback，不触发 early stop 是预期行为——该特性主要面向 io_uring 异步场景。

### 10.4 配置与统计

**SearchConfig 新增字段**：

```cpp
struct SearchConfig {
    // ... 已有字段 ...
    bool early_stop = true;  // 启用/禁用 Early Stop
};
```

设为 `false` 可强制探测所有 nprobe 个聚类，用于需要精确结果的场景。

**SearchStats 新增字段**：

```cpp
struct SearchStats {
    // ... 已有字段 ...
    bool early_stopped = false;      // 是否触发了 Early Stop
    uint32_t clusters_skipped = 0;   // 跳过的聚类数（0 = 未触发）
};
```

统计信息通过 `SearchResults::stats()` 访问。

### 10.5 触发后流程

Early Stop 触发后：
1. 记录 `early_stopped = true` 和 `clusters_skipped` 到统计
2. 跳出探测循环
3. **不取消已提交的 I/O**——FinalDrain 正常消费所有待处理完成事件
4. FetchMissingPayloads 和 AssembleResults 照常执行

### 10.6 修改文件

| 文件 | 变更 |
|------|------|
| `include/vdb/query/search_context.h` | SearchConfig 新增 `early_stop`，SearchStats 新增 `early_stopped`/`clusters_skipped` |
| `include/vdb/query/search_results.h` | 新增 `SearchStats stats_` 成员和访问器 |
| `src/query/overlap_scheduler.cpp` | ProbeAndDrainInterleaved 循环中加入 early stop 检查 + `sr.stats() = ctx.stats()` |
| `tests/query/early_stop_test.cpp` | 新增 3 个测试（StatsConsistency, NoEarlyStopWithSingleProbe, DisabledGivesExactResults） |
| `tests/query/overlap_scheduler_test.cpp` | 4 个精确结果测试添加 `config.early_stop = false` |
| `CMakeLists.txt` | 新增 `test_early_stop` 目标 |

### 10.7 测试覆盖

| 测试 | 验证内容 |
|------|----------|
| `StatsConsistency` | early_stopped=true 时 clusters_skipped>0，反之=0 |
| `NoEarlyStopWithSingleProbe` | nprobe=1 时不可能触发（无剩余聚类可跳过） |
| `DisabledGivesExactResults` | early_stop=false 时结果与暴力搜索完全一致 |

---

## 11. Phase 10：CRC 自适应 Early Stop

### 11.1 设计动机

Phase 9 的 Early Stop 使用固定阈值 `d_k`（ConANN 全局参考距离）判断是否提前终止。问题：`d_k` 是粗粒度的全局统计量，对不同查询的最优 nprobe 缺乏自适应能力。

CRC（Conformal Risk Control）引入统计保证的自适应 early-stop：离线标定 λ̂ 阈值，在线按查询动态判断停止时机。核心保证：FNR ≤ α（用户指定的目标假阴率上限）。

### 11.2 算法流程

**离线标定（CrcCalibrator）**：

```
输入: 标定查询集 + IVF 聚类数据
  ↓
① 计算每个查询在每个 nprobe 步骤的 top-K 距离 (raw_scores) 和 top-K ID 预测 (predictions)
  ↓
② 划分: calib_set (50%) / tune_set (10%) / test_set (40%)
  ↓
③ 归一化: nonconf_score = (d_kth - d_min) / (d_max - d_min)
  ↓
④ 正则化 (RAPS): E = (1 - nonconf) + λ_reg · max(0, probed - k_reg)
  ↓
⑤ BrentSolve: 在 [0, 1] 上找 λ̂ 使 calib_set 上 FNR ≈ α
  ↓
⑥ 评估: 在 test_set 上验证 actual_fnr, avg_probed, recall@K
  ↓
输出: CalibrationResults {lamhat, kreg, reg_lambda, d_min, d_max}
```

**在线查询（CrcStopper）**：

```cpp
// O(1) per call, no allocations
bool ShouldStop(uint32_t probed_count, float current_kth_dist) const {
    float nonconf = clamp((current_kth_dist - d_min) * d_range_inv, 0, 1);
    float E = (1 - nonconf) + reg_lambda * max(0, probed - kreg);
    return (E / max_reg_val) > lamhat;
}
```

### 11.3 核心文件

**`include/vdb/index/crc_stopper.h`** — 在线决策器

```cpp
struct CalibrationResults {
    float lamhat;        // λ̂: Brent 根查找的停止阈值
    uint32_t kreg;       // k_reg: RAPS 免费探测配额
    float reg_lambda;    // λ_reg: RAPS 排名惩罚系数
    float d_min, d_max;  // 归一化边界
};

class CrcStopper {
public:
    CrcStopper(const CalibrationResults& params, uint32_t nlist);
    bool ShouldStop(uint32_t probed_count, float current_kth_dist) const;
};
```

**`include/vdb/index/crc_calibrator.h`** + **`src/index/crc_calibrator.cpp`** — 离线标定器

```cpp
class CrcCalibrator {
public:
    struct Config {
        float alpha = 0.1f;       // 目标 FNR 上限
        uint32_t top_k = 10;
        float calib_ratio = 0.5f;
        float tune_ratio = 0.1f;
        uint64_t seed = 42;
    };

    struct EvalResults {
        float actual_fnr;     // 测试集实际 FNR
        float avg_probed;     // 平均探测聚类数
        float recall_at_1/5/10;
        uint32_t test_size;
    };

    // 精确 L2 距离标定
    static pair<CalibrationResults, EvalResults> Calibrate(
        const Config& config, const float* queries, ...);

    // RaBitQ 估计距离标定
    static pair<CalibrationResults, EvalResults> CalibrateWithRaBitQ(
        const Config& config, const float* queries, ...,
        const RotationMatrix& rotation);
};
```

### 11.4 测试覆盖

| 测试 | 文件 | 验证内容 |
|------|------|----------|
| BasicCalibration | `crc_calibrator_test.cpp` | λ̂ ∈ (0, 1], kreg=1, d_min < d_max |
| FnrBound | 同上 | actual_fnr ≤ alpha + tolerance |
| Determinism | 同上 | 同参数同数据 → 完全相同结果 |
| LargerAlphaFewerProbes | 同上 | α 越大 → avg_probed 越少 |
| CrcStopperBasic | `crc_stopper_test.cpp` | 堆未满时不停止，探测足够后停止 |
| Monotonicity | 同上 | 距离越小越倾向停止 |
| ProbedCountEffect | 同上 | 探测越多越倾向停止 |

---

## 12. Phase 11：CRC 标定与距离空间解耦

### 12.1 问题诊断

Phase 10 的 `CalibrateWithRaBitQ` 计算分数使用 RaBitQ 估计距离，但 GT（ground truth）来自精确 L2 距离。RaBitQ top-K 与精确 L2 top-K 的重合率仅 30–50%，导致：

```
FNR ≥ 50% (永远) → BrentSolve 返回 λ̂ = 1.0 → CRC early-stop 永不触发
```

### 12.2 解决方案

**核心思路**：使 CRC 标定成为纯分数域操作。GT 从 `predictions[nlist-1]`（全探测结果）派生，消除跨距离空间的 GT 不匹配。

```
改前:  RaBitQ 分数 vs 精确 L2 GT  →  跨空间比较 → FNR ≥ 50%
改后:  RaBitQ 分数 vs RaBitQ 全探测 GT → 同空间自一致 → FNR 正常
```

### 12.3 API 变更

**新增核心 API** — 距离空间无关的纯分数标定：

```cpp
/// Per-query scores — distance-space agnostic data unit
struct QueryScores {
    std::vector<float> raw_scores;                  // [nlist] kth-dist at each step
    std::vector<std::vector<uint32_t>> predictions; // [nlist][≤K] top-K IDs at each step
};

/// Core calibration: GT derived from predictions[nlist-1]
static pair<CalibrationResults, EvalResults> Calibrate(
    const Config& config,
    const vector<QueryScores>& all_scores,
    uint32_t nlist);
```

**重构现有 API** — 作为 wrapper 调用核心：

```cpp
// Exact L2 wrapper: 计算分数 → 调用核心 Calibrate
static pair<CalibrationResults, EvalResults> Calibrate(
    const Config& config,
    const float* queries, uint32_t num_queries, Dim dim,
    const float* centroids, uint32_t nlist,
    const vector<ClusterData>& clusters);

// RaBitQ wrapper: 计算分数 → 调用核心 Calibrate (移除 ground_truth 参数)
static pair<CalibrationResults, EvalResults> CalibrateWithRaBitQ(
    const Config& config,
    const float* queries, uint32_t num_queries, Dim dim,
    const float* centroids, uint32_t nlist,
    const vector<ClusterData>& clusters,
    const RotationMatrix& rotation);
```

### 12.4 构建时分数预计算

**IvfBuilder 新增 Phase E**：在 `WriteIndex()` 之后，如果 `crc_top_k > 0` 且提供了 `calibration_queries`：

```
Phase E: CRC Score Precomputation
  ① 构建 ClusterData（包含 RaBitQ codes）
  ② CrcCalibrator::ComputeScoresRaBitQ(calibration_queries, ...) → 全探测
  ③ CrcCalibrator::WriteScores("crc_scores.bin", scores, nlist, top_k)
  ④ CrcParams 写入 segment.meta（FlatBuffers）
```

**IvfBuilderConfig 新增字段**：

```cpp
uint32_t crc_top_k = 0;  // 0 = 跳过 CRC 分数预计算
```

### 12.5 QueryScores 序列化

二进制格式（`crc_scores.bin`）：

```
Header (20 bytes):
  magic:       uint32 = 0x43524353 ("CRCS")
  version:     uint32 = 1
  num_queries: uint32
  nlist:       uint32
  top_k:       uint32

Per query (× num_queries):
  raw_scores:   float[nlist]
  predictions:  uint32[nlist × top_k]  (不足 top_k 用 UINT32_MAX 填充)
```

### 12.6 FlatBuffers schema 变更

```flatbuffers
table CrcParams {
    scores_file: string;               // 相对路径: "crc_scores.bin"
    num_calibration_queries: uint32;
    calibration_top_k: uint32;
}
```

在 `SegmentMeta` 中新增 `crc_params: CrcParams;` 字段（`conann_params` 之后，`clusters` 之前）。

### 12.7 Benchmark 适配

**`bench_e2e.cpp`** CRC 路径重构：

- **主路径**：从索引目录加载 `crc_scores.bin` → `Calibrate(scores, nlist)`
- **回退路径**：若 `crc_scores.bin` 不存在，内联 RaBitQ 标定（无需原始向量或外部 GT）
- **移除**：暴力 GT 计算、CRC 相关的 `all_vectors` 加载

**其他 benchmark**：`bench_crc_overlap.cpp`、`bench_conann_crc.cpp` 移除 `ground_truth` 参数。

### 12.8 修改文件汇总

| 文件 | 变更 |
|------|------|
| `include/vdb/index/crc_calibrator.h` | QueryScores 公开、新增核心 Calibrate API、ComputeScoresRaBitQ、WriteScores/ReadScores |
| `src/index/crc_calibrator.cpp` | 核心实现、wrapper 重构、序列化、GT 从 predictions[nlist-1] 派生 |
| `schema/segment_meta.fbs` | 新增 CrcParams 表 |
| `include/vdb/index/ivf_builder.h` | 新增 crc_top_k 配置 |
| `src/index/ivf_builder.cpp` | Phase E: CRC 分数预计算 + CrcParams 写入 meta |
| `CMakeLists.txt` | vdb_index PRIVATE 链接 vdb_crc |
| `benchmarks/bench_e2e.cpp` | 加载 crc_scores.bin + 回退路径 |
| `benchmarks/bench_crc_overlap.cpp` | 移除 gt_topk 参数 |
| `benchmarks/bench_conann_crc.cpp` | 移除 gt 参数 |
| `tests/index/crc_calibrator_test.cpp` | 移除 gt 参数、新增 ScoresBasedCalibrate 和 WriteReadScoresRoundTrip 测试 |
| `tests/schema/schema_test.cpp` | 适配 CrcParams 新字段 |

---

## 13. Benchmark 体系

### 13.1 目录结构

Benchmark 从 `tests/index/` 独立至 `benchmarks/` 目录，受 `VDB_BUILD_BENCHMARKS` CMake 选项控制。

### 13.2 可用 Benchmark

| 可执行文件 | 源文件 | 功能 | 依赖 |
|-----------|--------|------|------|
| `bench_e2e` | `bench_e2e.cpp` | 端到端搜索：Build → Query → Recall → CRC early-stop | vdb_query, vdb_io, vdb_crc |
| `bench_ivf_quality` | `bench_ivf_quality.cpp` | IVF+RaBitQ 精度评估（Spearman 相关性、ConANN 三分类分布） | vdb_index |
| `bench_rabitq_accuracy` | `bench_rabitq_accuracy.cpp` | RaBitQ 距离估算精度 + d_k/ε_ip 标定验证 | vdb_index, vdb_io |
| `bench_rabitq_diagnostic` | `bench_rabitq_diagnostic.cpp` | RaBitQ 诊断工具（详细误差分析） | vdb_index, vdb_io |
| `bench_conann_recall` | `bench_conann_recall.cpp` | ConANN 三分类 recall 评估 | vdb_index, vdb_io |
| `bench_conann_crc` | `bench_conann_crc.cpp` | ConANN + CRC 联合标定效果 | vdb_crc, vdb_index, vdb_io |
| `bench_crc_overlap` | `bench_crc_overlap.cpp` | CRC early-stop 与全探测的 top-K 重叠率分析 | vdb_crc, vdb_index, vdb_io |

### 13.3 构建 Benchmark

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DVDB_BUILD_BENCHMARKS=ON
cmake --build build -j$(nproc)
```

### 13.4 数据集

| 数据集 | 路径 | 向量数 | 维度 | 用途 |
|--------|------|--------|------|------|
| coco_1k | `/home/zcq/VDB/data/coco_1k` | 1000 | 512 | 快速验证 |
| coco_5k | `/home/zcq/VDB/data/coco_5k` | 5000 | 512 | 完整评估 |

数据集结构：
```
data/coco_Nk/
  ├── image_embeddings.npy    # [N, 512] float32 图像向量
  ├── text_embeddings.npy     # [N, 512] float32 文本向量（跨模态查询）
  ├── image_ids.npy           # [N] int64 图像 ID
  └── metadata.jsonl          # N 行 JSON 元数据
```

---

## 14. 构建与测试

### 构建命令

```bash
cd VectorRetrival
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)
```

### 运行所有测试

```bash
ctest --test-dir build --output-on-failure
```

### 当前测试状态（37 个可执行文件）

```
Phase 0 Common:
  test_types              ✅
  test_status             ✅
  test_aligned_alloc      ✅
  test_distance           ✅

Phase 1 Schema:
  test_schema             ✅
  test_record             ✅

Phase 2 Columns:
  test_column_chunk       ✅
  test_column_store       ✅

Phase 3 SIMD:
  test_distance_l2        ✅
  test_bit_unpack         ✅
  test_prefix_sum         ✅  (含多流前缀和)
  test_transpose          ✅  (8×N 转置)
  test_popcount           ✅

Phase 3 Codec:
  test_bitpack_codec      ✅
  test_dict_codec         ✅

Phase 4 RaBitQ:
  test_rabitq_rotation    ✅
  test_rabitq_encoder     ✅
  test_rabitq_estimator   ✅

Phase 5 Storage:
  test_address_column     ✅  (含多流解码一致性)
  test_data_file          ✅
  test_cluster_store      ✅
  test_segment            ✅

Phase 6 Index:
  test_conann             ✅
  test_crc_stopper        ✅  (Phase 10 新增)
  test_crc_calibrator     ✅  (Phase 10 新增, Phase 11 更新)
  test_ivf_index          ✅
  test_ivf_builder        ✅
  test_payload_pipeline   ✅  (Phase 8 新增)

Phase 7 Query:
  test_distance           ✅
  test_pread_fallback_reader  ✅
  test_io_uring_reader        ✅
  test_result_collector       ✅
  test_buffer_pool            ✅
  test_rerank_consumer        ✅
  test_overlap_scheduler      ✅

Phase 8 IO:
  test_npy_reader         ✅
  test_jsonl_reader       ✅

Phase 9 Early Stop:
  test_early_stop         ✅  (StatsConsistency, NoEarlyStopWithSingleProbe, DisabledGivesExactResults)
```

### CMake Library Targets

| Target | Sources | Dependencies |
|--------|---------|-------------|
| `vdb_common` | INTERFACE | — |
| `vdb_schema` | Generated FlatBuffers | flatbuffers |
| `vdb_columns` | columns/*.cpp | vdb_common, Threads |
| `vdb_simd` | simd/*.cpp (distance_l2, bit_unpack, prefix_sum, popcount, transpose) | vdb_common |
| `vdb_codec` | codec/*.cpp (bitpack, dict) | vdb_simd |
| `vdb_rabitq` | rabitq/*.cpp (rotation, encoder, estimator) | vdb_simd |
| `vdb_storage` | storage/*.cpp (address_column, data_file_*, cluster_store, segment) | vdb_columns, vdb_codec, vdb_rabitq |
| `vdb_crc` | index/crc_calibrator.cpp | vdb_simd, vdb_rabitq, GSL (PRIVATE) |
| `vdb_index` | index/*.cpp (conann, ivf_index, ivf_builder) | vdb_storage, vdb_rabitq, vdb_simd, vdb_schema, vdb_crc (PRIVATE) |
| `vdb_query` | query/*.cpp (overlap_scheduler, rerank_consumer, ...) + common/distance.cpp | vdb_index, vdb_storage, vdb_simd, vdb_rabitq, liburing |
| `vdb_io` | io/*.cpp (npy_reader, jsonl_reader) | vdb_common |

---

## 15. 延迟优化项

### Phase 3 延迟项

| ID | 描述 | 优先级 | 状态 |
|----|------|--------|------|
| PHASE3-001 | AVX-512 L2 距离路径 | 低 | 未动 |
| PHASE3-002 | bit_unpack 2/4/8-bit AVX2 特化 | 中 | 未动 |
| PHASE3-003 | prefix_sum 16/32-wide SIMD 路径 | 低 | 未动 |
| PHASE3-004 | BitpackCodec::Encode SIMD 写路径 | 低 | 未动 |

### Phase 4 延迟项

| ID | 描述 | 优先级 | 状态 |
|----|------|--------|------|
| PHASE4-001 | 多位 RaBitQ（M=2,4,8） | **高** | 未动 — 1-bit Spearman ~0.75 |
| PHASE4-002 | AVX-512 VPOPCNTDQ 路径 | 低 | 未动 |
| PHASE4-003 | 编码器内存预分配/对象池 | 中 | 未动 |
| PHASE4-004 | Hadamard 旋转性能 benchmark | 低 | 已实现，待 benchmark |
| PHASE4-005 | 大维度 Gram-Schmidt 数值稳定性 | 中 | 未动 |

### Phase 6 延迟项

| ID | 描述 | 优先级 | 状态 |
|----|------|--------|------|
| PHASE6-001 | K-means++ 初始化 | — | ✅ 已实现 |
| PHASE6-002 | io_uring fallback reader | — | ✅ 已实现（Phase 7） |
| PHASE6-003 | 多线程 K-means | 低 | 未动 |
| PHASE6-004 | 索引批量更新（增量构建） | 低 | 未动 |

### Phase 7 延迟项

| ID | 描述 | 优先级 | 状态 |
|----|------|--------|------|
| PHASE7-001 | 多线程查询（MPMC 无锁队列） | 中 | 未动 |
| PHASE7-002 | Centroid 预热 / mmap 预加载 | 低 | 未动 |
| PHASE7-003 | 查询结果缓存 | 低 | 未动 |

---

## 16. 关键设计决策记录

| 决策 | 选择 | 理由 |
|------|------|------|
| RaBitQ 算法 | 随机旋转 + 符号量化 | 论文原始方法，无偏估计 |
| ConANN 阈值 | 加性 d_k ± 2ε | 全局预计算，稳定性好（改自原计划的乘性方案） |
| K-means 初始化 | K-means++ | 更好的收敛性 |
| 平衡聚类 | 容量受限贪心 | 簇大小一致性 |
| 记录存储 | 行存（row-major） | 单条 pread 读取完整记录 |
| 文件访问 | pread / io_uring | 线程安全 + 异步高吞吐 |
| 页对齐 | 4KB 可配置 | 地址压缩 + Direct I/O 兼容 |
| AddressColumn 解码 | 多流垂直并行 SIMD（K=8） | 8 流并行约 50% 操作减少 |
| base_offset 类型 | uint32_t 页索引 | 16TB@4KB 足够，核心循环全程 uint32 |
| .clu 文件版本 | v4 | base_offsets u64→u32 格式不兼容 |
| 查询调度 | 单线程交错 probe/drain | 简化并发模型，避免锁竞争 |
| safein_all_threshold | 256KB | 小记录一次读取 VEC+PAYLOAD，大记录分开读 |
| Payload 存储 | 行存 per-record（非列存） | 单列场景下与列存等价，实现简单 |
| PayloadFn 回调 | 延迟生成 | Build 时按需产生 payload，无需预先加载全部数据 |
| Payload 列约定 | INT64 id + BYTES/STRING data | 支持 recall 验证 + 多模态原始数据 |
| FlatBuffers 序列化 | SegmentMetaBuilder 模式 | 避免位置参数错误，字段名显式可见 |
| Early Stop 阈值 | d_k（ConANN 全局参考距离） | 复用已有校准参数，无需引入新阈值 |
| Early Stop 检查时机 | ProbeCluster 之后 | 机会性检查，不主动 drain，依赖 WaitAndPoll 自然消费 |
| Early Stop 与 PreadFallback | 不触发是预期行为 | PreadFallback 同步 I/O 导致 WaitAndPoll 被跳过，该特性面向 io_uring |
| CRC 自适应 early-stop | RAPS + Brent root-finding | 统计保证 FNR ≤ α，比固定阈值 d_k 更灵活 |
| CRC GT 派生 | predictions[nlist-1]（全探测结果） | 消除跨距离空间 GT 不匹配，RaBitQ 空间自一致 |
| CRC 分数预计算 | 构建时全探测 + 序列化 | 离线标定无需原始向量，支持迭代调参 |
| CRC 序列化格式 | 固定 K padding + UINT32_MAX 哨兵 | 避免变长记录，简化随机访问 |
| vdb_crc 独立库 | GSL PRIVATE 链接 | 隔离 GSL 依赖，不传播到 vdb_index 消费者 |
| Benchmark 独立目录 | benchmarks/ + VDB_BUILD_BENCHMARKS | 与单元测试分离，可选编译 |

---

## 17. 接手开发指南

### 17.1 当前完成状态

**Phase 0–11 全部完成**：
- ✅ 基础设施（Status、Types、SIMD、Codec、ColumnStore）
- ✅ RaBitQ 1-bit 量化（Rotation、Encoder、Estimator）
- ✅ Storage 层（AddressColumn、DataFile、ClusterStore、Segment）
- ✅ IVF 索引（ConANN、IvfIndex、IvfBuilder）
- ✅ 查询流水线（AsyncReader、OverlapScheduler、RerankConsumer）
- ✅ Payload 数据管道（PayloadFn、FlatBuffers 持久化、端到端测试）
- ✅ IO 加载器（NpyReader、JsonlReader）
- ✅ Early Stop 聚类探测提前终止（固定阈值 d_k + CRC 自适应）
- ✅ CRC 自适应 Early Stop（CrcCalibrator + CrcStopper，统计保证 FNR ≤ α）
- ✅ CRC 标定与距离空间解耦（构建时分数预计算 + 纯分数域标定）
- ✅ Benchmark 体系独立（7 个 benchmark 可执行文件）
- ✅ 37 个测试可执行文件

### 17.2 立即可做的任务

1. **存储格式 v6**（进行中）
   - .clu 双区域布局：Region 1 FastScan blocks + Region 2 ExRaBitQ entries
   - 详见 `openspec/changes/rabitq-storage-v6/`

2. **FastScan + AVX-512**（下一步）
   - VPSHUFB batch-32 Stage 1 + 全 SIMD 文件 AVX-512
   - 详见 `openspec/changes/fastscan-avx512/`

3. **多线程查询**（中期扩展）
   - MPMC 无锁队列 + 线程池
   - 当前单线程瓶颈：CPU-bound 的 RaBitQ EstimateBatch

### 17.3 代码风格与约定

- **命名**：类 `PascalCase`，函数 `PascalCase`，变量 `snake_case`，成员 `trailing_underscore_`
- **文件**：头文件 `.h`，实现 `.cpp`，测试 `_test.cpp`，benchmark `bench_*.cpp`
- **错误处理**：所有 I/O 和解析操作返回 `Status` 或 `StatusOr<T>`
- **SIMD**：`VDB_USE_AVX2` / `VDB_USE_AVX512` 宏控制编译路径（3 级：AVX-512 → AVX2 → 标量）
- **测试**：GoogleTest，每个模块独立 test executable，`ctest` 一键运行
- **C++ 标准**：C++17（`std::filesystem`、`std::string_view`、structured bindings）

### 17.4 已知限制

1. **多 bit RaBitQ + FastScan 已集成**：Stage 1 使用 FastScan VPSHUFB batch-32（14-bit 查询精度），Stage 2 使用 ExRaBitQ 4-bit（MAE=0.006）。v7 存储格式已上线，AVX-512 路径已添加。ConANN margin 公式尚未适配 14-bit 精度。
2. **单线程查询**：当前仅支持单线程协作式调度。
3. **无 WAL**：仅支持批量离线构建，不支持在线增量插入。
4. **内存占用**：整个 RaBitQ codes 必须加载到内存。超大规模数据集需分片。
5. **行存 Payload**：单列等价列存，但多列场景下需要读取完整记录。未来可扩展为列存。

---

## 18. Phase 12：ExRaBitQ 对齐

> **Change**: `exrabitq-alignment` (已归档)
> **日期**: 2026-03-31 ~ 2026-04-01

### 问题

原始多 bit RaBitQ 实现在大数据集（deep1m）上 recall@10 仅 0.15，与原作者 Extended-RaBitQ 的 0.95 差距巨大。根因分析发现三个算法缺陷：

1. **缺少 xipnorm 校正因子**：使用固定 `1/√D` 缩放内积，而非每向量独立的校正
2. **均匀 bin 量化**：直接切分 [-1,1] 为等宽区间，而非最大化量化保真度
3. **Benchmark 迭代慢**：每次重跑 KMeans

### 实施内容

**Step 0: 预计算聚类支持**
- bench_rabitq_accuracy / bench_vector_search 添加 `--centroids`/`--assignments` 参数
- 加载 faiss 预计算的 .fvecs/.ivecs，跳过 KMeans

**Step 1: xipnorm 校正因子**
- `RaBitQCode` 新增 `float xipnorm` 字段
- Encoder 编码后计算 `xipnorm = 1 / Σ(recon[i] × o'[i])`
- Estimator 距离公式：`ip_est = ip_raw × xipnorm`（替代固定 `1/√D`）
- 移除 LUT 查表机制

**Step 2: fast_quantize 优化量化**
- 对 `|o'|` 用优先队列搜索最优缩放因子 t，最大化 `⟨ō_q, ō'⟩ / ‖ō_q‖`
- 符号翻转编码：正分量存 `code_abs`，负分量存 `(2^M-1) - code_abs`
- **双存储架构**：`code[]`（均匀 bit-plane，Stage 1 popcount）+ `ex_code[]/ex_sign[]`（fast_quantize，Stage 2 精确估计）

**Step 3: AVX2 SIMD 加速**
- `simd::IPExRaBitQ()` 函数：float query × uint8 code_abs × uint8 sign
- AVX2 实现（8 floats/iter），后续已添加 AVX-512 路径（16 floats/iter）

### 结果

```
deep1m, bits=4, K=4096:

距离精度:
                    原始         +xipnorm     +fast_quantize
  Stage 2 MAE:     0.123        0.043        0.006

bench_rabitq_accuracy (暴力排序 recall):
  Stage 1 recall@10: 0.40
  Stage 2 recall@10: 0.96

bench_vector_search (nprobe=50, 100 queries):
  recall@10 = 0.9730
  False SafeOut = 0
  SafeOut: 78% → 99% (S1+S2 两阶段)
  Uncertain: 22% → 1%
  latency: 2.9ms/query (AVX2)
```

### 关键设计决策

- **双存储而非共享 bit-plane**：Stage 1 (popcount) 需要 plane 0 = sign bit，Stage 2 (fast_quantize) 需要全 M bits。两者不可共存在 M 个 plane 中，因此分开存储。
- **xipnorm 不是 1/√D**：1-bit 下 `⟨ō_q, ō'⟩ ≈ √D` 使得 `xipnorm ≈ 1/√D`；M-bit 下量化向量长度随向量变化，必须每向量独立计算。
- **LUT 被淘汰**：LUT 将 code 映射到 bin 中心再乘 query，精度不如直接 float×int 点积 + xipnorm 校正。

### 文件变更

| 文件 | 变更 |
|------|------|
| `include/vdb/rabitq/rabitq_encoder.h` | RaBitQCode 新增 `ex_code`, `ex_sign`, `xipnorm` |
| `src/rabitq/rabitq_encoder.cpp` | fast_quantize 优先队列量化 |
| `include/vdb/rabitq/rabitq_estimator.h` | 移除 LUT，新增 xipnorm 参数 |
| `src/rabitq/rabitq_estimator.cpp` | 直接点积 + xipnorm 校正 |
| `include/vdb/simd/ip_exrabitq.h` | 新 SIMD 函数 |
| `src/simd/ip_exrabitq.cpp` | AVX2 + AVX-512 实现 |
| `benchmarks/bench_rabitq_accuracy.cpp` | --centroids/--assignments + Stage 2 recall |
| `benchmarks/bench_vector_search.cpp` | 同上 |

---

## 19. Phase 13：SuperKMeans 集成

> **Change**: `superkmeans-integration` (已归档)
> **日期**: 2026-04-01

### 问题

1. ivf_builder 的手写 KMeans 单线程 O(N×K×D)，高维数据慢
2. 6 个 benchmark 各自拷贝了 ~50 行 KMeans 代码
3. `thrid-party/conann/` 从未使用

### 实施内容

- **CMake 集成**：`add_subdirectory(thrid-party/SuperKMeans)` → INTERFACE 库自动带入 OpenMP + OpenBLAS + Eigen
- **ivf_builder 重写**：`RunKMeans()` 从 170 行 KMeans++ + balance 替换为 35 行 SuperKMeans + precomputed loading
- **移除 balance_factor**：`IvfBuildConfig` 删除 `balance_factor` 字段及全部相关代码（~80 行容量约束逻辑）
- **Benchmark 清理**：6 个文件删除 static `KMeans()`，替换为 `RunSuperKMeans()` 包装调用
- **第三方库清理**：删除 `thrid-party/conann/`

### SuperKMeans 性能

```
COCO100k (N=100K, D=512, K=512):
  SuperKMeans: 4.6 sec (PDX 剪枝 97%)
  faiss:       28.8 sec
  加速比:      6.3x

deep1m (N=1M, D=96, K=4096):
  SuperKMeans: 125 sec (D < 128, 无 PDX 剪枝)
  faiss:       ~15 sec
  结论: D < 128 时 SuperKMeans 无优势，用 faiss 预计算
```

### 第三方库现状

```
thrid-party/
├── SuperKMeans/          ← add_subdirectory 引入，链接到 vdb_index
│   └── extern/Eigen/     ← bundled Eigen 3.4.0
└── Extended-RaBitQ/      ← 参考实现，不参与 build
```

---

## 20. Phase 14：存储格式 v7 双区域布局

> **Change**: `rabitq-storage-v6` (已归档)
> **日期**: 2026-04-01

### 问题

v6 格式 record-major 存储（`[code_words|norm|sum_x]` 逐向量排列）有两个问题：
1. **不兼容 FastScan**：VPSHUFB 要求 codes 按 dim-group-of-4 跨 32 向量交错排列，record-major 需运行时重打包
2. **混合关注**：1-bit sign codes（Stage 1 顺序扫描）和 ExRaBitQ codes（Stage 2 随机访问 Uncertain）交错存放，浪费带宽

### 设计

Per-cluster block 改为三区域：

```
Region 1: FastScan Blocks（Stage 1 顺序扫描）
  Block b (32 vectors):
    packed_codes: D×4 bytes（nibble-interleaved, VPSHUFB 友好）
    norm_oc[32]:  128 bytes
  Block size: D×4 + 128 bytes
  Total: ceil(N/32) blocks

Region 2: ExRaBitQ Entries（Stage 2 随机访问，仅 bits > 1）
  Vec v: ex_code[D] + ex_sign[D] + xipnorm(f32)
  Entry size: 2×D + 4 bytes

Region 3: Address packed data + Mini-trailer（不变）
```

### 实施内容

**数据结构**
- `RaBitQConfig` 新增 `storage_version` 字段
- `ParsedCluster` 新增 `fastscan_blocks`/`fastscan_block_size`/`num_fastscan_blocks` + `exrabitq_entries`/`exrabitq_entry_size` + helper methods (`norm_oc()`, `ex_code()`, `ex_sign()`, `xipnorm()`)
- `ClusterLookupEntry` 新增 `num_fastscan_blocks`、`exrabitq_region_offset`
- 版本常量 6 → 7

**Pack/Unpack 工具**
- `PackSignBitsForFastScan()`：将 N 个 RaBitQCode 的 sign bits 打包为 VPSHUFB block-32 nibble-interleaved 格式
- `UnpackSignBitsFromFastScan()`：逆操作，从 packed block 提取单个向量的 code words
- 排列 `kPerm = {0,8,1,9,2,10,3,11,4,12,5,13,6,14,7,15}` 与原论文 AVX-512 FastScan 一致

**Writer 重写**
- `WriteVectors()`：分组 32 → PackSignBitsForFastScan → 写 Region 1 → 写 Region 2
- `EndCluster()`：记录 `num_fastscan_blocks` 和 `exrabitq_region_offset` 到 lookup entry

**Reader 重写**
- `ParseClusterBlock()`：解析 Region 1/2/3 边界，填充 ParsedCluster 新字段
- `EnsureClusterLoaded()`：codes_length = Region 1 + Region 2
- `LoadCode()`：从 packed block 反提取 sign bits（inverse of pack）

**查询路径**
- `OverlapScheduler::ProbeCluster()`：按 FastScan block 遍历，不再逐条记录

### 结果

```
deep1m, nprobe=50, 100 queries:

bits=4: recall@10 = 0.9730, False SafeOut = 0, latency = 3.528ms
bits=1: recall@10 = 0.9730, False SafeOut = 0, latency = 2.746ms

所有 recall 值与 v6 baseline 完全一致 — 存储格式变更 bit-exact。
```

### 文件变更

| 文件 | 变更 |
|------|------|
| `include/vdb/common/types.h` | `RaBitQConfig` +`storage_version` |
| `include/vdb/query/parsed_cluster.h` | 双区域字段 + helper methods |
| `include/vdb/storage/cluster_store.h` | `ClusterLookupEntry` 新字段, `fastscan_block_bytes()`, `exrabitq_entry_size()` |
| `include/vdb/storage/pack_codes.h` | 新增 `PackSignBitsForFastScan`, `UnpackSignBitsFromFastScan`, `FastScanPackedSize` |
| `src/storage/pack_codes.cpp` | 新文件：VPSHUFB packing 实现 |
| `src/storage/cluster_store.cpp` | Writer + Reader 全面重写为 v7 |
| `src/query/overlap_scheduler.cpp` | FastScan block 遍历 |

---

## 21. Phase 15：FastScan + AVX-512

> **Change**: `fastscan-avx512` (已归档)
> **日期**: 2026-04-01

### 问题

Stage 1 两个性能瓶颈：
1. **逐向量串行**：`PopcountXor` 每次处理 1 个向量，无跨向量并行
2. **1-bit 查询精度**：查询也被 sign 量化到 1 bit → MAE=0.105 → 22% Uncertain 需 Stage 2

FastScan（原论文 Extended-RaBitQ）使用 VPSHUFB 同时处理 32 个向量，且查询保留 14-bit 精度。

### 设计

```
当前:                               FastScan:
for vec in cluster:                  for block of 32 vecs:
  hamming = PopcountXor(q, code)       result[32] = VPSHUFB(LUT, codes)
  ip = 1 - 2*hamming/D                // 32 ips in ~D/4 VPSHUFB ops
  // 1 vec at a time                  // 32 vecs simultaneously
```

**Part A: FastScan 核心**

1. `QuantizeQuery14Bit(q', quant_out, dim) → width`：14-bit 对称量化查询向量
   - `width = max(|q'|) / 8191`，`quant[i] = round(q'[i] / width)`
   - 14-bit 是 VPSHUFB 8-bit LUT 约束下的最大精度：4 个值之和 `≤ 4×8191 = 32764 < 32767`

2. `BuildFastScanLUT(quant_query, lut_out, dim) → shift`：构建 VPSHUFB 查找表
   - 每组 4 维构建 16 项 LUT：`LUT[j] = Σ quant_query[k]` (k ∈ j 的 set bits)
   - 使用 `lowbit(j) = j & (-j)` 递推
   - 平移到无符号，拆分 lo/hi byte planes，按 AVX-512 128-bit lane 布局存放
   - LUT 大小：`dim × 8` bytes（两个 byte plane）

3. `AccumulateBlock(packed_codes, lut, result, dim)`：VPSHUFB batch-32 累积
   - AVX-512：每次迭代处理 64 bytes codes + 2×64 bytes LUT（4 个 sub-quantizer）
   - Two-plane 累加器（lo/hi bytes）避免 int16 溢出
   - 最终合并：`result[v] = lo_plane[v] + (hi_plane[v] << 8)` → 32 个 uint32_t
   - AVX2 fallback：同逻辑，32 bytes/迭代

4. `EstimateDistanceFastScan(pq, packed_codes, block_norms, count, out_dist)`：
   - AccumulateBlock → 反量化 `ip_raw = (accu + shift) × width`
   - 内积估计 `⟨q̄,ô⟩ = (2 × ip_raw − sum_q) / √D`（与 EstimateDistanceAccurate 等价）
   - 距离 `dist = norm_oc² + norm_qc² − 2 × norm_oc × norm_qc × ip_est`

**Part B: AVX-512 SIMD 升级**

| 文件 | AVX-512 关键变化 | 关键 intrinsics |
|------|-----------------|----------------|
| `distance_l2.cpp` | 16 floats/iter | `_mm512_fmadd_ps`, `_mm512_reduce_add_ps` |
| `ip_exrabitq.cpp` | 16 floats/iter, mask blend | `_mm512_cvtepu8_epi32`, `_mm512_cmpneq_epi32_mask`, `_mm512_mask_blend_ps` |
| `popcount.cpp` | VPOPCNTDQ 硬件 popcount | `_mm512_popcnt_epi64`, 8 uint64/iter |
| `bit_unpack.cpp` | 16 bits/iter | `_mm512_srlv_epi32` |
| `prefix_sum.cpp` | 保留 AVX2（stride-8 API） | — |
| `transpose.cpp` | 保留 AVX2（8×8 block API） | — |

编译时 3 层 dispatch：`#if VDB_USE_AVX512 / #elif VDB_USE_AVX2 / #else scalar`

CMake：`-DVDB_USE_AVX512=ON` 同时定义 `VDB_USE_AVX2=1` 并添加 `-mavx512vpopcntdq`。

### 结果

```
deep1m, nprobe=50, 100 queries, -DVDB_USE_AVX512=ON:

bits=4: recall@10 = 0.9730, False SafeOut = 0, latency = 3.446ms
bits=1: recall@10 = 0.9730, False SafeOut = 0, latency = 2.869ms

recall 与 popcount baseline 完全一致。
37/38 单元测试通过（1 个预存在的 flaky test_conann）。
```

### 文件变更

| 文件 | 变更 |
|------|------|
| `CMakeLists.txt` | AVX-512 flags + `fastscan.cpp` 加入 `vdb_simd` |
| `include/vdb/simd/fastscan.h` | 新增：`AccumulateBlock`, `BuildFastScanLUT`, `QuantizeQuery14Bit` |
| `src/simd/fastscan.cpp` | 新增：AVX-512 + AVX2 + scalar 三路实现 |
| `include/vdb/rabitq/rabitq_estimator.h` | `PreparedQuery` +FastScan 字段; +`EstimateDistanceFastScan` |
| `src/rabitq/rabitq_estimator.cpp` | PrepareQuery 新增 step 7 (LUT 构建); FastScan 距离估算 |
| `src/query/overlap_scheduler.cpp` | ProbeCluster 替换为 FastScan batch 调用 |
| `src/simd/distance_l2.cpp` | +AVX-512 path (16 floats/iter) |
| `src/simd/ip_exrabitq.cpp` | +AVX-512 path (mask blend) |
| `src/simd/popcount.cpp` | +AVX-512 VPOPCNTDQ path |
| `src/simd/bit_unpack.cpp` | +AVX-512 path (16 bits/iter) |

---

## 22. 待实施项

1. **FastScan ConANN 分类集成**：当前 FastScan 计算距离后仍用 popcount 公式的 ConANN margin。应适配 14-bit 精度的 margin 公式，预期 Uncertain% 从 43% 大幅降低。
2. **运行时 ISA dispatch**：当前 `#ifdef` 编译时选择 AVX2/AVX-512。生产环境需 `cpuid` 检测 + 函数指针分发。
3. **Stage 2 FastScan**：当前 Stage 2 仍用逐向量 `IPExRaBitQ`。可用 `accumulate_one_block_high_acc` 参考实现批处理 Uncertain 向量。
4. **延迟优化**：当前 bits=1 latency 2.87ms，目标 < 2ms。瓶颈在 I/O（io_uring 提交）而非 CPU 估算。
  - 16×16 block transpose
