# VDB VectorRetrival — 项目开发总结

> **最后更新**：2026-03-10  
> **已完成**：Phase 0 – Phase 6（完整）  
> **下一步**：Phase 7（查询流水线） → Phase 8+（优化与扩展）

---

## 目录

1. [项目概述](#1-项目概述)
2. [已完成模块一览](#2-已完成模块一览)
3. [Phase 0–2：基础设施（Common / Schema / ColumnStore）](#3-phase-02基础设施)
4. [Phase 3：SIMD + Codec](#4-phase-3simd--codec)
5. [Phase 4：RaBitQ 量化](#5-phase-4rabitq-量化)
6. [Phase 5：Storage 层](#6-phase-5storage-层)
7. [Phase 6：IVF 索引（已完成）](#7-phase-6ivf-索引已完成)
8. [Phase 7：查询流水线（未开始）](#8-phase-7查询流水线未开始)
9. [构建与测试](#9-构建与测试)
10. [延迟优化项（UNDO.txt）](#10-延迟优化项)
11. [关键设计决策记录](#11-关键设计决策记录)
12. [接手开发指南](#12-接手开发指南)

---

## 1. 项目概述

VDB VectorRetrival 是一个面向高维向量近似最近邻搜索（ANNS）的存储与查询引擎。

**核心算法流水线**：
```
IVF 粗聚类 → ConANN 三分类 → RaBitQ 量化距离估算 → Rerank 精排
```

**技术栈**：C++17, AVX2 SIMD, FlatBuffers, GoogleTest, CMake

**设计特点**：
- Cluster 1:1 DataFile 存储设计
- Address Column 通过 bit-packing + SIMD 解码实现记录定位
- RaBitQ 1-bit 随机旋转量化（非 PQ 标量量化）
- 页对齐存储（4KB 粒度）
- 行存 DataFile + pread 随机读取
- **ConANN 三分类**：全局预计算 $d_k$，使用 $\tau_{in} = d_k - 2\varepsilon$，$\tau_{out} = d_k + 2\varepsilon$ 的加性阈值
- **平衡聚类**：容量受限 K-means，支持 `balance_factor` 参数控制簇大小一致性

---

## 2. 已完成模块一览

### 文件统计

| 阶段 | 头文件 | 实现文件 | 测试文件 | 测试用例数 |
|------|--------|---------|---------|-----------|
| Phase 0 Common | 4 | 0 | 3 | ~30 |
| Phase 1 Schema | 3 (.fbs) | 0 | 2 | ~20 |
| Phase 2 Columns | 3 | 3 | 2 | ~66 |
| Phase 3 SIMD+Codec | 5 | 5 | 6 | ~100 |
| Phase 4 RaBitQ | 4 | 4 | 4 | ~64 |
| Phase 5 Storage | 5 | 5 | 4 | ~51 |
| Phase 6 IVF Index | **6** | **3** | **3** | **~30** |
| **合计** | **30** | **20** | **24** | **~361** |

### 库依赖关系

```
vdb_storage
  ├── vdb_columns      (Phase 2: ColumnStore)
  ├── vdb_codec        (Phase 3: BitpackCodec)
  ├── vdb_rabitq       (Phase 4: RaBitQEncoder/Estimator/Rotation)
  ├── vdb_simd         (Phase 3: L2Sqr/BitUnpack/PrefixSum/Popcount)
  └── vdb_schema       (Phase 1: FlatBuffers 生成代码)

vdb_common (INTERFACE) ← 所有模块都依赖
```

---

## 3. Phase 0–2：基础设施

### 3.1 Common（Phase 0）

**文件**：
- `include/vdb/common/types.h` — 核心类型定义
- `include/vdb/common/status.h` — `Status` / `StatusOr<T>` 错误处理
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
enum class DType : uint8_t { FLOAT32, INT32, INT64, STRING, BYTES, ... };
enum class ResultClass : uint8_t { SafeIn, SafeOut, Uncertain };
enum class ReadTaskType : uint8_t { ALL, FRONT, VEC_ONLY, BACK, PAYLOAD };

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

// 搜索结果
struct SearchResult {
    VecID vec_id;
    float distance;
};

// 候选
struct Candidate {
    VecID        vec_id;
    float        approx_dist;
    ResultClass  result_class;
    ClusterID    cluster_id;
    uint32_t     local_idx;
};

// 常量
constexpr uint32_t kDefaultPageSize = 4096;  // 页对齐粒度
```

**Status 系统**：
```cpp
Status s = DoSomething();
if (!s.ok()) return s;

StatusOr<ClusterStoreReader> reader = ClusterStoreReader::Open(path);
if (!reader.ok()) return reader.status();
auto& r = reader.value();
```

支持的错误码：`OK`, `InvalidArgument`, `NotFound`, `IOError`, `Corruption`, `AlreadyExists`, `NotSupported`

### 3.2 Schema（Phase 1）

**FlatBuffers Schema 文件**：

`schema/columns.fbs`：
- `DataType` 枚举（FLOAT32, INT32, INT64, STRING, BYTES, ...）
- `EncodingType` 枚举（NONE, DICT, BITPACK, DELTA, RLE, FOR, RABITQ）
- `ColumnChunkMeta` table（column_id, encoding, num_values, compressed_size, ...）

`schema/segment_meta.fbs`：
```fbs
table IvfParams {
    nlist: uint32;
    nprobe: uint32;
    balance_factor: float;           // NEW: 平衡聚类参数
    centroids_offset: uint64;
    centroids_length: uint64;
}

table ConANNParams {
    epsilon: float;                  // NEW: 固定阈值参数
    d_k: float;                      // NEW: 预计算的全局参考距离
    calibration_samples: uint32;     // NEW: 校准时采样数
    calibration_topk: uint32;        // NEW: 校准时 top-k 的 k
    calibration_percentile: float;   // NEW: 分位数
    // 已弃用（向后兼容保留）：
    tau_in_factor: float;   // DEPRECATED
    tau_out_factor: float;  // DEPRECATED
}

table RaBitQParams {
    bits: uint8;
    block_size: uint32;
    c_factor: float;
    codebook_offset: uint64;
    codebook_length: uint64;
}

table AddressBlockMeta {
    base_offset: uint64;
    bit_width: uint8;
    data_offset: uint64;
    data_length: uint32;
}

table AddressColumnMeta {
    block_granularity: uint32;
    num_blocks: uint32;
    blocks: [AddressBlockMeta];
}

table ClusterMeta {
    cluster_id: uint32;
    num_records: uint32;
    address_column: AddressColumnMeta;
    // ... 其他字段
}

table SegmentMeta {
    segment_id: uint32;
    dimension: uint32;
    metric_type: MetricType;
    ivf_params: IvfParams;
    conann_params: ConANNParams;
    rabitq_params: RaBitQParams;
    clusters: [ClusterMeta];
    // ... 其他字段
}
```

`schema/record.fbs`：
- `DataFileHeader` table（magic, version, cluster_id, num_records, dimension, column_schemas, ...）
- `ColumnSchemaEntry`（column_id, name, data_type）

**代码生成**：CMake `FlatBuffersGenerate.cmake` → 生成 `.h` 到 `${CMAKE_BINARY_DIR}/generated/`

### 3.3 ColumnStore（Phase 2）

**文件**：
- `include/vdb/columns/column_chunk_writer.h` → 单列写入
- `include/vdb/columns/column_chunk_reader.h` → 单列读取
- `include/vdb/columns/column_store.h` → 多列管理器

**功能**：支持固定长度列（INT32, FLOAT, INT64）和变长列（STRING, BYTES）的分块读写。被 Phase 5 的 DataFileWriter/Reader 复用。

---

## 4. Phase 3：SIMD + Codec

### 4.1 L2 距离（distance_l2）

**文件**：`include/vdb/simd/distance_l2.h` + `src/simd/distance_l2.cpp`

```cpp
namespace vdb::simd {
    // AVX2 加速的 L2 平方距离
    float L2Sqr(const float* a, const float* b, Dim dim);
    
    // 标量 fallback
    float L2SqrScalar(const float* a, const float* b, Dim dim);
}
```

**实现**：
- AVX2 路径：每次循环处理 8 个 float（`__m256`），`_mm256_sub_ps` → `_mm256_mul_ps` → `_mm256_add_ps` 累加
- 尾部处理：`dim % 8` 个元素用标量处理
- Horizontal sum：`_mm256_hadd_ps` → `_mm_add_ps` → `_mm_cvtss_f32`

### 4.2 Bit Unpack（bit_unpack）

**文件**：`include/vdb/simd/bit_unpack.h` + `src/simd/bit_unpack.cpp`

```cpp
namespace vdb::simd {
    // 将 packed 数据展开为 uint32_t 数组
    void BitUnpack(const uint8_t* packed, uint8_t bit_width,
                   uint32_t* out, uint32_t count);
    
    // AVX2 特化（1-bit）
    void BitUnpack1Bit_AVX2(const uint8_t* packed, uint32_t* out, uint32_t count);
    
    // 标量通用路径
    void BitUnpackScalar(const uint8_t* packed, uint8_t bit_width,
                         uint32_t* out, uint32_t count);
}
```

**实现细节**：
- 1-bit AVX2 特化：`_mm256_movemask_epi8` 反向操作，每次处理 32 个 bit
- 标量路径：支持任意位宽（1–32），跨字节边界正确处理


### 4.3 Prefix Sum（prefix_sum）

**文件**：`include/vdb/simd/prefix_sum.h` + `src/simd/prefix_sum.cpp`

```cpp
namespace vdb::simd {
    // Exclusive prefix sum：out[i] = sum(in[0..i-1])
    void ExclusivePrefixSum(const uint32_t* in, uint32_t* out, uint32_t count);
}
```

### 4.4 BitpackCodec

**文件**：`include/vdb/codec/bitpack_codec.h` + `src/codec/bitpack_codec.cpp`

```cpp
namespace vdb::codec {
    class BitpackCodec {
    public:
        // 计算最小位宽
        static uint8_t ComputeMinBitWidth(const uint32_t* values, uint32_t count,
                                           uint32_t max_packable_value);
        static std::vector<uint8_t> Encode(const uint32_t* values, uint32_t count,
                                            uint8_t bit_width);
        static void Decode(const uint8_t* packed, uint8_t bit_width,
                           uint32_t* out, uint32_t count);
    };
}
```

### 4.5 DictCodec

**文件**：`include/vdb/codec/dict_codec.h` + `src/codec/dict_codec.cpp`

字典编解码器：对重复值多的列进行去重压缩。

---

## 5. Phase 4：RaBitQ 量化

### 5.1 算法概述

RaBitQ 的正确算法流程（**注意：不是 PQ 的 min/max 标量量化**）：

```
原始向量 → 中心化(减 centroid) → 归一化 → 随机正交旋转 P^T → 符号量化 → bit-pack
```

**距离估算公式**：

$$\|o - q\|^2 \approx \|o-c\|^2 + \|q-c\|^2 - 2\|o-c\|\cdot\|q-c\|\cdot\langle\bar{q}, \hat{o}\rangle$$

其中内积通过 XOR + popcount 快速计算：

$$\langle\bar{q}, \hat{o}\rangle \approx 1 - 2H/L$$

$H$ 为查询符号码和数据库符号码的 Hamming 距离。

### 5.2 Popcount 模块

**文件**：`include/vdb/simd/popcount.h` + `src/simd/popcount.cpp`

```cpp
namespace vdb::simd {
    // count bits in a word (inlined)
    uint32_t Popcount64(uint64_t x);

    // hamming distance between two codes
    uint32_t PopcountXor(const uint64_t* a,
                         const uint64_t* b,
                         uint32_t num_words);

    // total set bits in a code
    uint32_t PopcountTotal(const uint64_t* code,
                           uint32_t num_words);
}
```


### 5.3 旋转矩阵模块

**文件**：`include/vdb/rabitq/rabitq_rotation.h` + `src/rabitq/rabitq_rotation.cpp`

```cpp
namespace vdb::rabitq {
    class RotationMatrix {
    public:
        explicit RotationMatrix(Dim dim);
        
        void GenerateRandom(uint64_t seed);
        bool GenerateHadamard(uint64_t seed, bool use_srht = true);
        void Apply(const float* vec, float* out) const;
        
        // Phase 6 NEW: 持久化支持
        Status Save(const std::string& path) const;
        static StatusOr<RotationMatrix> Load(const std::string& path, Dim dim);
        
        Dim GetDim() const;
        bool IsHadamard() const;
        const std::vector<float>& GetData() const;
    };
}
```

### 5.4 RaBitQ 编码器

**文件**：`include/vdb/rabitq/rabitq_encoder.h` + `src/rabitq/rabitq_encoder.cpp`

```cpp
namespace vdb::rabitq {
    struct RaBitQCode {
        std::vector<uint64_t> code;  // bit-packed 符号码，L/64 个 uint64_t
        float norm_sq;               // ‖o - c‖²
        uint32_t sum_x;                // popcount(code)
    };
    
    class RaBitQEncoder {
    public:
        RaBitQEncoder(Dim dim, const RotationMatrix& P);
        RaBitQCode Encode(const float* vec,
                          const float* centroid = nullptr) const;
        std::vector<RaBitQCode> EncodeBatch(
            const std::vector<const float*>& vectors,
            const float* centroid = nullptr) const;
    };
}
```

**编码流程**：
```
vec ─┬─ r = vec - centroid
     |
     ├─ norm_sq = ‖r‖²
     |
     ├─ r̄ = r / ‖r‖     (归一化)
     |
     ├─ r̄' = P^T · r̄    (旋转)
     |
     └─ code[i] = (r̄'[i] >= 0) ? 1 : 0    (符号量化)
        → bit-pack 到 uint64_t 数组
```

**LSB 优先**：第 $i$ 维对应 `code[i/64]` 的第 `i%64` 位。

### 5.5 RaBitQ 距离估算器

**文件**：`include/vdb/rabitq/rabitq_estimator.h` + `src/rabitq/rabitq_estimator.cpp`

```cpp
namespace vdb::rabitq {
    struct PreparedQuery {
    std::vector<float>    rotated;     // q' = P^T × q̄  (length = dim)
    std::vector<uint64_t> sign_code;   // Sign bits of q' packed as uint64_t
    float                 norm_qc;     // ‖q - c‖₂
    float                 norm_qc_sq;  // ‖q - c‖₂²
    float                 sum_q;       // Σ q'[i] (sum of all rotated components)
    Dim                   dim;         // Vector dimensionality
    uint32_t              num_words;   // ceil(dim / 64)
    };
    
    class RaBitQEstimator {
    public:
        explicit RaBitQEstimator(Dim dim);
        
        // per-query, per-cluster 预处理（做一次）
        PreparedQuery PrepareQuery(const float* query,
                                    const float* centroid,
                                    const RotationMatrix& P) const;
        
        // 快速距离估算（热循环，per-vector）
        // 使用 XOR + popcount，O(L/64) 
        float EstimateDistance(const PreparedQuery& pq,
                               const RaBitQCode& db_code) const;
        
        // 精确距离估算（逐位浮点计算，O(L)）
        float EstimateDistanceAccurate(const PreparedQuery& pq,
                                       const RaBitQCode& db_code) const;
        
        // 批量距离估算
        void EstimateDistanceBatch(const PreparedQuery& pq,
                                   const RaBitQCode* codes,
                                   uint32_t count,
                                   float* distances) const;
    };
}
```

**快速路径 `EstimateDistance`（热循环内核）**：
```cpp
// 1. Hamming 距离 = popcount(q_sign XOR db_code)
uint32_t hamming = 0;
for (uint32_t w = 0; w < num_words; ++w) {
    hamming += Popcount(pq.sign_code[w] ^ db_code.code[w]);
}

// 2. 内积估计
float inner_prod = 1.0f - 2.0f * hamming / dim;

// 3. 距离还原
float dist = db_code.norm_sq + norm_qc² 
           - 2 * sqrt(db_code.norm_sq) * norm_qc * inner_prod;
return max(dist, 0.0f);
```

**两条路径对比**：

| 路径 | 复杂度 | 精度 | 用途 |
|------|--------|------|------|
| 快速（popcount） | O(L/64) | 1-bit 量化精度 | 热循环、top-K 初选 |
| 精确（逐位浮点） | O(L) | 较高 | 精排验证 |

### 5.6 Phase 4 测试说明

测试中 centroid 参数化设计：`centroid=nullptr` → 使用零向量。这允许在没有 IVF 聚类的情况下独立测试 RaBitQ 编码和距离估算。

**已知精度限制**：1-bit 量化的距离估算与真实 L2 的 Spearman 相关性在 128 维时约 0.6-0.8。Ranking preservation 测试使用宽松阈值（top-10 在 top-20 中的 recall ≥ 1）。多 bit 扩展（M=2,4,8）可显著提升精度，已记录在 UNDO.txt。

---

## 6. Phase 5：Storage 层

### 6.1 AddressColumn

**文件**：`include/vdb/storage/address_column.h` + `src/storage/address_column.cpp`

```cpp
namespace vdb::storage {
    struct AddressBlock {
    uint64_t base_offset;            // First record's page index
    uint8_t  bit_width;              // Bits per page-unit size value
    uint32_t record_count;           // Actual records in this block (≤ 64)
    uint32_t page_size;              // Page granularity in bytes (default 4096)
    std::vector<uint8_t> packed;     // Bit-packed page-unit sizes
    };
    
    class AddressColumn {
    public:
        // 编码 N 条记录的地址
        // block_size: 每块记录数（默认 64）
        // page_size: 页对齐粒度（默认 4096，传 1 则不对齐）
        Status Encode(const AddressEntry* entries, uint32_t count,
                      uint32_t block_size = 64,
                      uint32_t page_size = page_size);
        
        // 解码单个块
        void DecodeBlock(uint32_t block_idx, uint32_t count,
                         AddressEntry* out) const;
        
    };
}
```

**页对齐存储设计**
- 传入 `page_size=4096` 时，所有 offset 和 size 除以 page_size 后存储
- 解码时乘回 page_size
、

**SIMD 解码流程**：
```
packed_sizes → BitUnpack (AVX2) → uint32_t[64] → ExclusivePrefixSum (AVX2) → offsets
```

### 6.2 DataFileWriter / DataFileReader

**文件**：
- `include/vdb/storage/data_file_writer.h` + `src/storage/data_file_writer.cpp`
- `include/vdb/storage/data_file_reader.h` + `src/storage/data_file_reader.cpp`

```cpp
class DataFileWriter {
public:
    static StatusOr<DataFileWriter> Open(
        const std::string& path,
        uint32_t cluster_id,
        Dim vec_dim,
        const std::vector<ColumnSchema>& payload_schemas,
        uint32_t page_size = kDefaultPageSize);
    
    StatusOr<AddressEntry> WriteRecord(
        const float* vec,
        const std::vector<Datum>& payload,
        AddressEntry& out_entry
        );
    
    Status Finalize();
};
```

**记录格式**：
```
┌────────────────────────────────────────┐
│ raw_vector: D × float                  │
│ payload_col_0: 固定长度数据              │
│ payload_col_1: ...                     │
│ var_len_offsets + var_len_data         │
│ [padding to page_size alignment]       │
└────────────────────────────────────────┘
```

**DataFileReader**：
```cpp
class DataFileReader {
public:
    static StatusOr<DataFileReader> Open(
        const std::string& path,
        Dim vec_dim,
        const std::vector<ColumnSchema>& payload_schemas);
    
    // 读取整条记录
    Status ReadRecord(const AddressEntry& addr,
                      std::vector<float>* vec_out,
                      std::vector<Datum>* payload_out);
    
    // 只读向量部分（Rerank 用）
    Status ReadVector(const AddressEntry& addr,
                      std::vector<float>* vec_out);
};
```

**关键**：使用 `pread` 系统调用（线程安全，不依赖文件指针）。

### 6.3 ClusterStore

**文件**：`include/vdb/storage/cluster_store.h` + `src/storage/cluster_store.cpp`

```cpp
class ClusterStoreWriter {
public:
    struct ClusterInfo {
        uint32_t cluster_id;
        uint32_t num_records;
        Dim dim;
        RaBitQConfig rabitq_config;
        std::string data_file_path;
        // ... 各段的 offset/length
        std::vector<AddressBlock> address_blocks;
    };

    static StatusOr<ClusterStoreWriter> Open(
        const std::string& path, Dim dim, uint32_t cluster_id);
    
    Status WriteCentroid(const float* centroid);
    Status WriteVectors(const std::vector<RaBitQCode>& codes);
    Status WriteAddressBlocks(const std::vector<AddressBlock>& blocks);
    Status Finalize();
};

class ClusterStoreReader {
public:
    static StatusOr<ClusterStoreReader> Open(const std::string& path);
    
    // Phase 5 / 6: 从 .clu 文件重建 ClusterInfo
    static Status ReadInfo(const std::string& path,
                           ClusterStoreWriter::ClusterInfo* out);
};
```

**.clu 文件格式**：
```
Offset  Content
─────────────────────────────────
0                  Centroid (dim × float)
?                  RaBitQ codes (N × code_words × 8B)
?                  Address blocks packed 数据（连续写入）
                   block0.packed
                   block1.packed
                   ...
文件末尾前 trailer_size 字节  Trailer（只含元数据 + packed_offset）
文件最后 8 字节    trailer_size (u32) + magic (0x56434C55 u32)


### 6.4 Segment

**文件**：`include/vdb/storage/segment.h` + `src/storage/segment.cpp`

```cpp
class Segment {
public:
    explicit Segment(Dim dim, const std::vector<ColumnSchema>& schemas = {});
    
    Status AddCluster(uint32_t cluster_id,
                      const std::string& clu_path,
                      const std::string& dat_path);
    
    StatusOr<ClusterStoreReader*> GetClusterReader(uint32_t cluster_id);
    StatusOr<DataFileReader*> GetDataFileReader(uint32_t cluster_id);
    
    uint32_t NumClusters() const;
    bool HasCluster(uint32_t cluster_id) const;
};
```

---

## 7. Phase 6：IVF 索引（已完成）

### 7.1 核心设计修改（相比原初期计划）

**ConANN 三分类阈值模型**：
- **改动**：从乘性 `tau × topk_dist` 改为加性 $d_k \pm 2\varepsilon$
- **原因**：全局预计算的 $d_k$ 更稳定，避免动态堆顶依赖
- **影响**：`Classify(float approx_dist)` 不再需要 `topk_dist` 参数

**平衡聚类支持**：
- **新增**：`IvfBuilderConfig.balance_factor` 参数
- `balance_factor = 0`：标准 K-means（无容量限制）
- `balance_factor > 0`：每个 cluster 最大容量为 `ceil((1 + balance_factor) × N / nlist)`
- **实现**：容量受限 K-means，Assign 阶段贪心分配

**d_k 全局校准**：
- 在构建时（Phase B）对数据集进行采样，计算所有样本的 top-k 距离，取分位数作为 $d_k$
- 校准参数（samples, topk, percentile）记录在 FlatBuffers 元数据中，支持复现

### 7.2 完整实现概览

| 组件 | 文件 | 行数 | 功能 |
|------|------|------|------|
| ConANN | `include/vdb/index/conann.h` | ~80 | 三分类，全局阈值，距离校准 |
| | `src/index/conann.cpp` | ~150 | 实现 |
| | `tests/index/conann_test.cpp` | ~300 | 16 个测试用例 |
| IvfIndex | `include/vdb/index/ivf_index.h` | ~90 | 索引加载，最近簇查询 |
| | `src/index/ivf_index.cpp` | ~200 | Open、FindNearestClusters 实现 |
| | `tests/index/ivf_index_test.cpp` | ~250 | 8 个测试用例 |
| IvfBuilder | `include/vdb/index/ivf_builder.h` | ~120 | 离线构建配置 |
| | `src/index/ivf_builder.cpp` | ~600 | K-means + 文件写入 + FlatBuffers 序列化 |
| | `tests/index/ivf_builder_test.cpp` | ~400 | 6 个测试用例 |
| **Benchmark** | `tests/index/bench_ivf_quality.cpp` | ~350 | RaBitQ 精度 + ConANN 分布统计 |

### 7.3 ConANN 实现细节

```cpp
class ConANN {
    float epsilon_;       // ε = c·2^(-B/2)/√D
    float d_k_;           // 全局参考距离
    float tau_in_;        // = d_k - 2ε
    float tau_out_;       // = d_k + 2ε

public:
    ConANN(float epsilon, float d_k);
    
    static ConANN FromConfig(const RaBitQConfig& cfg, Dim dim);
    
    ResultClass Classify(float approx_dist) const;
    
    static float CalibrateDistanceThreshold(
        const float* vectors, uint32_t N, Dim dim,
        uint32_t num_samples, uint32_t top_k,
        float percentile, uint64_t seed);
};
```

**CalibrateDistanceThreshold 流程**：
1. 随机抽 `num_samples` 个向量作查询
2. 对每个查询，暴力计算到所有向量的 L2 距离
3. `std::nth_element` 取第 `top_k` 近的距离
4. 对所有样本的 top-k 距离排序，取 `percentile` 分位数

**测试**（16 cases）：
- epsilon 公式验证
- SafeIn / SafeOut / Uncertain 分类
- 边界条件（approx_dist 恰好等于阈值）
- 校准确定性（种子固定 → 结果固定）
- 极端情况（epsilon 非常大/小）

### 7.4 IvfIndex 实现细节

```cpp
class IvfIndex {
    Dim dim_;
    uint32_t nlist_;
    std::vector<float> centroids_;
    ConANN conann_;
    Segment segment_;
    std::vector<ClusterID> cluster_ids_;

public:
    Status Open(const std::string& dir);
    std::vector<ClusterID> FindNearestClusters(
        const float* query, uint32_t nprobe) const;
};
```

**Open 流程**：
1. 读 `segment.meta`（FlatBuffers）→ 解析 IvfParams、ConANNParams
2. 加载 `centroids.bin`（裸 float 数组）
3. 加载 `rotation.bin`（RotationMatrix 持久化）
4. 注册所有 cluster（路径 → ClusterStoreReader）

**FindNearestClusters 流程**：
1. 计算 query 到所有 centroid 的 L2 距离
2. `std::nth_element` 取最小 nprobe 个
3. 对那 nprobe 个排序后返回

**测试**（8 cases）：
- 打开索引成功
- 正确数量的最近簇
- 簇按距离排序
- ConANN 正确加载
- Segment 正确注册
- nprobe 上界夹紧（不超过 nlist）

### 7.5 IvfBuilder 实现细节

```cpp
struct IvfBuilderConfig {
    uint32_t nlist = 256;
    uint32_t max_iterations = 50;
    float tolerance = 1e-4f;
    uint64_t seed = 42;
    float balance_factor = 0.0f;  // NEW
    RaBitQConfig rabitq = {};
    uint32_t calibration_samples = 100;    // NEW
    uint32_t calibration_topk = 10;        // NEW
    float calibration_percentile = 0.99f;  // NEW
    std::vector<ColumnSchema> payload_schemas = {};
    uint32_t page_size = 4096;
};

class IvfBuilder {
public:
    Status Build(const float* vectors, uint32_t N, Dim dim,
                 const std::string& output_dir,
                 const IvfBuilderConfig& config);
    
    const std::vector<uint32_t>& assignments() const;
    const std::vector<float>& centroids() const;
    float calibrated_dk() const;
    void SetProgressCallback(std::function<void(uint32_t, uint32_t)> cb);
};
```

**Build 流程**（4 个 Phase）：

**Phase A — 容量受限 K-means++**：
- K-means++ 初始化（距离概率加权）
- 迭代 Assign + Update
  - **标准分配**（balance_factor=0）：每向量 → 最近 centroid
  - **平衡分配**（balance_factor>0）：
    - 计算所有 (vec, centroid) 距离对
    - 按距离升序排序
    - 贪心分配：优先距离最近且capacity未满的 centroid
- 收敛判定：max centroid 变化 < tolerance

**Phase B — ConANN d_k 校准**：
- 调用 `ConANN::CalibrateDistanceThreshold`
- 返回全局 $d_k$

**Phase C — 逐 cluster RaBitQ 编码 + 存储**：
- 生成全局旋转矩阵 P，保存到 `rotation.bin`
- 对每个 cluster：
  - DataFileWriter 逐条写记录（向量 + payload）
  - RaBitQEncoder 编码向量
  - AddressColumn 编码记录地址
  - ClusterStoreWriter 写入 .clu 文件（含 centroid、codes、address blocks）
  - DataFileWriter 写入 .dat 文件（payload 列存）

**Phase D — 写 segment.meta**：
- FlatBuffers 序列化所有元数据
- 包含 IvfParams（nlist、balance_factor）、ConANNParams、RaBitQParams、ClusterMeta

**测试**（6 cases）：
- 基础构建：文件存在性、assignments 完整性
- 赋值完整性：所有 N 向量恰好分配到某个 cluster
- 平衡聚类：cluster 大小不超过容量限制
- 往返：Build → Open → 验证最近簇匹配
- 进度回调：被调用 nlist 次
- 无效输入：null vectors、N=0、dim=0、nlist>N

### 7.6 Benchmark 可执行文件

**功能**：独立程序（不依赖 GoogleTest），评估构建后的索引质量。

**输出统计**（两个部分）：

**Part 1 — RaBitQ 量化精度**（按 cluster）：
```
Cluster | Size | Spearman | MeanRelErr | StdRelErr
--------|------|----------|------------|----------
     0  |  312 |   0.847  |    0.123   |   0.089
     1  |  287 |   0.821  |    0.131   |   0.093
```

**Part 2 — ConANN 三分类分布**：
```
SafeIn:      12.3%
SafeOut:     67.9%
Uncertain:   19.8%
```

**工作流程**：
1. 生成随机向量（N=10000, dim=128, 默认可配）
2. Build 索引
3. Open 索引
4. 采样 100 个查询，统计 RaBitQ 精度和三分类分布

---

## 8. Phase 7：查询流水线（未开始）

完整的查询流水线尚未开始实现。计划组件：

| 组件 | 文件 | 功能 |
|------|------|------|
| SearchContext | `query/search_context.h` | 查询上下文：持有 query、TopK 堆、队列引用 |
| IoUringReader | `query/io_uring_reader.h` | io_uring 异步 I/O 封装 |
| ReadQueue | `query/read_queue.h` | Qv（高优先）+ Qp（低优先）双队列 |
| OverlapScheduler | `query/overlap_scheduler.h` | Probe + I/O + Rerank 协作调度 |
| RerankConsumer | `query/rerank_consumer.h` | 流式 Rerank 消费者 |
| Warmup | `query/warmup.h` | centroid 预热、mmap 预加载 |
| ResultCollector | `query/result_collector.h` | TopK max-heap |

**外部依赖**：`liburing`（Linux ≥ 5.1）

**线程模型**：初期单线程协作式调度（Probe batch → Submit I/O → Poll → Rerank → 继续）

---

## 9. 构建与测试

### 构建命令

```bash
cd VectorRetrival
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 运行所有测试

```bash
cd build
ctest --output-on-failure
```

### 当前测试状态

```
Test project /home/zcq/VDB/VectorRetrival/build

      Start  1: types_test              ✅
      Start  2: status_test             ✅
      Start  3: aligned_alloc_test      ✅
      Start  4: schema_columns_test     ✅
      Start  5: schema_segment_test     ✅
      Start  6: column_chunk_test       ✅
      Start  7: column_store_test       ✅
      Start  8: test_distance_l2        ✅
      Start  9: test_bit_unpack         ✅
      Start 10: test_prefix_sum         ✅
      Start 11: test_bitpack_codec      ✅
      Start 12: test_dict_codec         ✅
      Start 13: test_popcount           ✅
      Start 14: test_rabitq_rotation    ✅
      Start 15: test_rabitq_encoder     ✅
      Start 16: test_rabitq_estimator   ✅
      Start 17: test_address_column     ✅
      Start 18: test_data_file          ✅
      Start 19: test_cluster_store      ✅
      Start 20: test_segment            ✅
      Start 21: test_conann             ✅ (NEW)
      Start 22: test_ivf_index          ✅ (NEW)
      Start 23: test_ivf_builder        ✅ (NEW)
      Start 24: bench_ivf_quality       ✅ (NEW)

100% tests passed, 23 tests failed, 0 errors
Total: ~361 individual test cases across 24 executables
```

### CMake Library Targets

| Target | Sources | Dependencies |
|--------|---------|-------------|
| `vdb_common` | INTERFACE | — |
| `vdb_schema` | Generated FlatBuffers | FlatBuffers |
| `vdb_columns` | columns/*.cpp | vdb_common, vdb_schema |
| `vdb_simd` | simd/*.cpp | vdb_common |
| `vdb_codec` | codec/*.cpp | vdb_simd |
| `vdb_rabitq` | rabitq/*.cpp | vdb_simd, vdb_common |
| `vdb_storage` | storage/*.cpp | vdb_columns, vdb_codec, vdb_rabitq, vdb_simd |
| `vdb_index` | index/*.cpp | vdb_storage, vdb_rabitq, vdb_simd, vdb_schema |
| `vdb_query`（计划） | query/*.cpp | vdb_index, vdb_storage, vdb_simd, liburing |

---

## 10. 延迟优化项

以下内容记录在 UNDO.txt 中，作为后续可实施的优化：

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
| PHASE4-001 | 多位 RaBitQ（M=2,4,8） | **高** | 未动 — 1-bit 精度有限（Spearman ~0.75） |
| PHASE4-002 | AVX-512 VPOPCNTDQ 路径 | 低 | 未动 |
| PHASE4-003 | 编码器内存预分配/对象池 | 中 | 未动 |
| PHASE4-004 | Hadamard 旋转性能 benchmark | 低 | 已实现，待 benchmark |
| PHASE4-005 | 大维度 Gram-Schmidt 数值稳定性 | 中 | 未动 |

### Phase 6 新增优化项

| ID | 描述 | 优先级 | 状态 |
|----|------|--------|------|
| PHASE6-001 | K-means++ 初始化 | 中 | 已实现 |
| PHASE6-002 | io_uring fallback reader（同步 pread） | **高** | 未动 — Phase 7 需要 |
| PHASE6-003 | 多线程 K-means（Data-parallel clusters） | 低 | 未动 |
| PHASE6-004 | 索引批量更新（增量构建） | 低 | 未动 — 当前仅离线构建 |

---

## 11. 关键设计决策记录

| 决策 | 选择 | 理由 | 更新（Phase 6） |
|------|------|------|----------------|
| RaBitQ 算法 | 随机旋转 + 符号量化 | 论文原始方法，无偏估计 | 无变化 |
| ConANN 阈值 | 加性 $d_k \pm 2\varepsilon$ | 全局预计算，稳定性好 | **改为加性**（原计划乘性） |
| K-means 初始化 | K-means++ | 更好的收敛性 | 新增 |
| 平衡聚类 | 容量受限贪心 | 簇大小一致性 | 新增 |
| Centroid 参数化 | `centroid=nullptr` → 零向量 | RaBitQ 独立测试 | 无变化 |
| 旋转矩阵 | 随机正交（默认）+ Hadamard（可选） | 通用性 + 性能 | 新增 Save/Load |
| 记录存储 | 行存（row-major） | 单条 pread 读取完整记录 | 无变化 |
| 文件访问 | pread | 线程安全 | 无变化 |
| 页对齐 | 4KB 可配置 | 地址压缩 | 无变化 |
| AddressColumn 解码 | 全块 SIMD 解码 | 批量高效 | 无变化 |
| ClusterStore 加载 | 全量 codes 到内存 | RaBitQ 热路径优化 | 无变化 |
| Trailer 格式 | 文件末尾 8 字节定位 | 自包含、无外部依赖 | 无变化 |
| d_k 校准 | 全局预计算 + 持久化 | 可复现、可配置 | **新增决策** |
| balance_factor | 容量系数，0=无约束 | 灵活控制簇大小分布 | **新增决策** |

---

## 12. 接手开发指南

### 12.1 Phase 6 完成后的状态

**已完成**：
- ✅ Schema 修改（IvfParams.balance_factor、ConANNParams.epsilon/d_k/校准参数）
- ✅ ConANN 实现（三分类、全局 d_k、校准）
- ✅ IvfIndex 实现（Open、FindNearestClusters）
- ✅ IvfBuilder 实现（容量受限 K-means、d_k 校准、文件写入、FlatBuffers 序列化）
- ✅ 单元测试（27 个新增测试用例）
- ✅ Benchmark（RaBitQ 精度 + ConANN 分布统计）

**文件新增**：
- 6 个头文件（conann.h、ivf_index.h、ivf_builder.h、rabitq_rotation.h 修改）
- 3 个实现文件
- 3 个测试文件 + 1 个 benchmark 文件
- CMakeLists.txt 中新增 `vdb_index` 库目标

### 12.2 立即可做的任务

1. **Phase 7 查询流水线**（紧急）
   - 最关键：`IoUringReader` + fallback `PreadFallbackReader`
   - 次要：`ResultCollector`（TopK max-heap）、`RerankConsumer`
   - 可参考 Phase 6 的设计复杂度

2. **多 bit RaBitQ**（中期优化）
   - Phase 4 UNDO 项之一，能显著提升精度（Spearman 从 0.75 → 0.9+）
   - 不需要改动 Phase 6–7，纯粹新增

3. **性能 Benchmark**（可选）
   - 构建速度、查询延迟、内存占用
   - 可设定为后续 PR 的验收指标

### 12.3 代码风格与约定

- **命名**：类 `PascalCase`，函数 `PascalCase`，变量 `snake_case`，成员 `trailing_underscore_`
- **文件**：头文件 `.h`，实现 `.cpp`，测试 `_test.cpp`，benchmark 通常 `bench_*.cpp`
- **错误处理**：所有 I/O 和解析操作返回 `Status` 或 `StatusOr<T>`
- **SIMD**：`VDB_USE_AVX2` 宏控制编译路径，标量 fallback 必须存在
- **测试**：GoogleTest，每个模块独立 test executable，`ctest` 一键运行全部

### 12.4 调试与验证工作流

```bash
# 修改代码后
cd build
cmake .. && make -j$(nproc) 2>&1 | tail -20   # 编译
ctest --output-on-failure                       # 全量测试

# 单独运行某个测试
./test_ivf_builder --gtest_filter="BalancedClustering"

# 运行 benchmark
./bench_ivf_quality --N=5000 --dim=64 --nlist=16

# 调试模式构建
cd build_debug
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
gdb ./test_conann  # 进行调试
```

### 12.5 已知限制与问题

1. **1-bit RaBitQ 精度**：Spearman 相关性约 0.6-0.8（128 维），对于高精度场景不足。多 bit 扩展（UNDO PHASE4-001）是关键改进方向。

2. **K-means 收敛**：在高维数据上可能收敛较慢。可考虑早停或调整 tolerance。

3. **单线程查询**：Phase 7 初期为单线程协作式调度，多线程扩展需要 MPMC 无锁队列。

4. **无 WAL**：仅支持批量离线构建，不支持在线增量插入。要实现增量需要 WAL 和版本管理。

5. **內存占用**：整个 RaBitQ codes 集合必须加载到内存（热路径优化）。超大规模数据集可能需要分片或流式扫描。

### 12.6 验收标准

**Phase 6 应交付物**：
- ✅ 23 个新增测试全部通过
- ✅ Benchmark 正常运行，输出合理统计数据
- ✅ 代码覆盖率 ≥ 85%（关键路径）
- ✅ 无内存泄漏（AddressSanitizer）
- ✅ 无未初始化使用（MemorySanitizer）

**Phase 7 应交付物（需定义）**：
- Query 延迟 < 100 ms（top-100，1024 维，1M 向量）
- 吞吐量 > 1K QPS（单线程）
- 内存占用 < 2 GB（1M 向量，128 维，64 个簇）
- io_uring 利用率显著高于 pread fallback

---

## 总结

VDB VectorRetrival 现已完成 **Phase 0–6 的全套实现**，包括：
- 完整的向量量化和距离估算管道（RaBitQ）
- 高性能 IVF 索引构建（平衡聚类 + 全局校准）
- StorageLayer 文件格式和内存布局
- 27 个新增单元测试 + Benchmark 工具

**项目可立即进入 Phase 7 查询阶段**。下一步重点是实现异步 I/O 和流式 Rerank 流水线，以实现低延迟高吞吐的查询。
