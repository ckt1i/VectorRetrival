# VDB VectorRetrival — 项目开发总结

> **最后更新**：2026-03-09  
> **已完成**：Phase 0 – Phase 5（Step 1 of Phase 6）  
> **下一步**：Phase 6 剩余 Steps（ConANN、IvfIndex、IvfBuilder）→ Phase 7（查询流水线）

---

## 目录

1. [项目概述](#1-项目概述)
2. [已完成模块一览](#2-已完成模块一览)
3. [Phase 0–2：基础设施（Common / Schema / ColumnStore）](#3-phase-02基础设施)
4. [Phase 3：SIMD + Codec](#4-phase-3simd--codec)
5. [Phase 4：RaBitQ 量化](#5-phase-4rabitq-量化)
6. [Phase 5：Storage 层](#6-phase-5storage-层)
7. [Phase 6：IVF 索引（进行中）](#7-phase-6ivf-索引进行中)
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
| **合计** | **24** | **17** | **21** | **~331** |

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
    centroids_offset: uint64;
    centroids_length: uint64;
}

table ConANNParams {
    tau_in_factor: float;
    tau_out_factor: float;
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
    size: uint32;
    centroid_offset: uint64;
    centroid_length: uint32;
    rabitq_data_offset: uint64;
    rabitq_data_length: uint64;
    address_column: AddressColumnMeta;
    data_file_path: string;
    norms_offset: uint64;
    norms_length: uint32;
}

table SegmentMeta {
    segment_id: uint32;
    dimension: uint32;
    num_vectors: uint64;
    metric_type: MetricType;
    ivf_params: IvfParams;
    conann_params: ConANNParams;
    rabitq_params: RaBitQParams;
    clusters: [ClusterMeta];
    column_schemas: [ColumnSchemaMeta];
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
    void BitUnpack1_AVX2(const uint8_t* packed, uint32_t* out, uint32_t count);
    
    // 标量通用路径
    void BitUnpackScalar(const uint8_t* packed, uint8_t bit_width,
                         uint32_t* out, uint32_t count);
}
```

**实现细节**：
- 1-bit AVX2 特化：`_mm256_movemask_epi8` 反向操作，每次处理 32 个 bit
- 标量路径：支持任意位宽（1–32），跨字节边界正确处理
- **Bug fix（Phase 5 期间）**：原始标量路径只处理 2 字节跨越，修复为支持任意字节跨越（17+ bit 值可以跨 3+ 字节）

### 4.3 Prefix Sum（prefix_sum）

**文件**：`include/vdb/simd/prefix_sum.h` + `src/simd/prefix_sum.cpp`

```cpp
namespace vdb::simd {
    // Exclusive prefix sum：out[i] = sum(in[0..i-1])
    void ExclusivePrefixSum(const uint32_t* in, uint32_t* out, uint32_t count);
    
    // AVX2 路径：每次处理 8 个 uint32_t
    void ExclusivePrefixSum_AVX2(const uint32_t* in, uint32_t* out, uint32_t count);
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
        
        // 编码（bit-pack）
        static std::vector<uint8_t> Encode(const uint32_t* values, uint32_t count,
                                            uint8_t bit_width);
        
        // 解码（调用 simd::BitUnpack）
        static void Decode(const uint8_t* packed, uint8_t bit_width,
                           uint32_t* out, uint32_t count);
    };
}
```

**Bug fix（Phase 5 期间）**：`Encode` 函数原始只处理值跨越至多 2 字节的情况，修复为任意位宽（1–32）的正确跨字节写入。

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
    // 单个 uint64_t 的 popcount
    VDB_FORCE_INLINE uint32_t Popcount(uint64_t x) {
        return __builtin_popcountll(x);
    }
    
    // 两个 uint64_t XOR 后的 popcount
    VDB_FORCE_INLINE uint32_t PopcountXor(uint64_t a, uint64_t b) {
        return Popcount(a ^ b);
    }
    
    // 批量 XOR + popcount
    uint32_t PopcountXorBatch(const uint64_t* a, const uint64_t* b,
                               uint32_t num_words);
    
    // AVX2 快速路径
    uint32_t PopcountXorBatch_AVX2(const uint64_t* a, const uint64_t* b,
                                    uint32_t num_words);
}
```

**AVX2 实现**：VPSHUFB nibble-lookup 方法，每 256-bit 一次迭代处理 4 个 uint64_t：
1. XOR 两个 256-bit 寄存器
2. 分离高低 nibble（4-bit）
3. VPSHUFB 查表得到每个 nibble 的 popcount
4. VPSADBW 水平求和

### 5.3 旋转矩阵模块

**文件**：`include/vdb/rabitq/rabitq_rotation.h` + `src/rabitq/rabitq_rotation.cpp`

```cpp
namespace vdb::rabitq {
    class RotationMatrix {
    public:
        explicit RotationMatrix(Dim dim);
        
        // 方法 1：完整随机正交矩阵（Gram-Schmidt QR）
        // O(L²) 存储 + O(L²) 乘法
        void GenerateRandom(uint64_t seed);
        
        // 方法 2：随机化 Hadamard 变换（SRHT）
        // O(L) 存储 + O(L log L) 乘法
        // 要求 L 为 2 的幂（自动 padding）
        bool GenerateHadamard(uint64_t seed, bool use_srht = true);
        
        // 应用旋转：out = P^T · vec
        void Apply(const float* vec, float* out) const;
        
        Dim GetDim() const;
        bool IsHadamard() const;
        const std::vector<float>& GetData() const;
    };
}
```

**GenerateRandom 实现**：
1. 用 `std::normal_distribution` 生成 L×L 随机矩阵
2. Gram-Schmidt 正交化（逐列投影消除 + 归一化）
3. 结果满足 $P^T P = I$

**GenerateHadamard 实现**：
1. Pad 维度到 2 的幂
2. 生成随机对角矩阵 $D$（±1）
3. Apply 时：先乘 $D$，再做 FWHT（Fast Walsh-Hadamard Transform），再乘 $1/\sqrt{L}$

**旋转矩阵在整个 Segment 内全局唯一**，构建时生成一次，查询时加载。

### 5.4 RaBitQ 编码器

**文件**：`include/vdb/rabitq/rabitq_encoder.h` + `src/rabitq/rabitq_encoder.cpp`

```cpp
namespace vdb::rabitq {
    // 量化后的向量代码
    struct RaBitQCode {
        std::vector<uint64_t> code;  // bit-packed 符号码，L/64 个 uint64_t
        float norm_sq;               // ‖o - c‖²
    };
    
    class RaBitQEncoder {
    public:
        RaBitQEncoder(Dim dim, const RotationMatrix& P);
        
        // 编码单条向量
        // centroid=nullptr 时使用零向量（测试用）
        RaBitQCode Encode(const float* vec,
                          const float* centroid = nullptr) const;
        
        // 批量编码
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
    // per-query 预计算结果
    struct PreparedQuery {
        std::vector<float> rotated;      // P^T · q̄（旋转后的归一化查询）
        std::vector<uint64_t> sign_code; // 符号量化的 bit 码
        float norm_qc;                   // ‖q - c‖
        Dim dim;
        uint32_t num_words;              // sign_code 的 uint64_t 数量
        float sum_q_prime;               // ∑ rotated[i]
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
        uint64_t base_offset;          // 块首记录的字节偏移
        uint8_t  bit_width;            // 每条 size 的位宽
        std::vector<uint8_t> packed;   // bit-packed sizes
    };
    
    class AddressColumn {
    public:
        // 编码 N 条记录的地址
        // block_size: 每块记录数（默认 64）
        // page_size: 页对齐粒度（默认 4096，传 1 则不对齐）
        Status Encode(const AddressEntry* entries, uint32_t count,
                      uint32_t block_size = 64,
                      uint32_t page_size = kDefaultPageSize);
        
        // 解码单个块
        void DecodeBlock(uint32_t block_idx, uint32_t count,
                         AddressEntry* out) const;
        
        // 随机查找
        AddressEntry Lookup(uint32_t record_idx) const;
        
        // 批量查找（同块共享解码）
        void BatchLookup(const uint32_t* ids, uint32_t count,
                         AddressEntry* out) const;
        
        uint32_t NumBlocks() const;
        const AddressBlock& GetBlock(uint32_t idx) const;
    };
}
```

**页对齐存储设计**（Phase 5 后期新增）：
- 传入 `page_size=4096` 时，所有 offset 和 size 除以 page_size 后存储
- 解码时乘回 page_size
- 好处：offset/size 数值变小，bit_width 更窄，压缩率更高
- 测试中传 `page_size=1` 保持字节级精度

**SIMD 解码流程**：
```
packed_sizes → BitUnpack (AVX2) → uint32_t[64] → ExclusivePrefixSum (AVX2) → offsets
```

### 6.2 DataFileWriter / DataFileReader

**文件**：
- `include/vdb/storage/data_file_writer.h` + `src/storage/data_file_writer.cpp`
- `include/vdb/storage/data_file_reader.h` + `src/storage/data_file_reader.cpp`

**DataFileWriter**：
```cpp
class DataFileWriter {
public:
    static StatusOr<DataFileWriter> Open(
        const std::string& path,
        uint32_t cluster_id,
        Dim vec_dim,
        const std::vector<ColumnSchema>& payload_schemas,
        uint32_t page_size = kDefaultPageSize);
    
    // 写入一条记录，返回 AddressEntry
    StatusOr<AddressEntry> WriteRecord(
        const float* vec,
        const std::vector<Datum>& payload = {});
    
    Status Finalize();
};
```

**记录格式（行存）**：
```
┌────────────────────────────────────────┐
│ raw_vector: D × float                 │
│ payload_col_0: 固定长度数据             │
│ payload_col_1: ...                     │
│ var_len_offsets + var_len_data          │
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

**Writer**：
```cpp
class ClusterStoreWriter {
public:
    struct ClusterInfo {
        uint32_t cluster_id;
        uint32_t num_records;
        Dim dim;
        // ... 各段的 offset/length
        std::vector<AddressBlock> address_blocks;
    };

    static StatusOr<ClusterStoreWriter> Open(
        const std::string& path, Dim dim, uint32_t cluster_id);
    
    Status WriteCentroid(const float* centroid);
    Status WriteVectors(const std::vector<RaBitQCode>& codes);
    Status WriteAddressBlocks(const std::vector<AddressBlock>& blocks);
    Status Finalize();
    
    const ClusterInfo& info() const;
};
```

**Reader**：
```cpp
class ClusterStoreReader {
public:
    static StatusOr<ClusterStoreReader> Open(const std::string& path);
    
    // Phase 6 新增：从 .clu 文件重建 ClusterInfo（不需要外部传入）
    static Status ReadInfo(const std::string& path,
                           ClusterStoreWriter::ClusterInfo* out);
    
    Dim GetDim() const;
    uint32_t NumRecords() const;
    const float* Centroid() const;
    const RaBitQCode& GetCode(uint32_t idx) const;
    AddressEntry GetAddress(uint32_t idx) const;
};
```

**.clu 文件格式**：
```
Offset  Content
─────────────────────────────────
0       Header: magic(8B "VDBCLUST") + version(4B) + dim(4B) + num_records(4B)
20      Centroid: D × float
20+4D   RaBitQ codes: N × (ceil(D/64) × uint64_t)
?       Norms: N × float
?       AddressBlocks: 序列化块数组
?       Trailer:
          cluster_id(u32) + dim(u32) + num_records(u32)
          + centroid_offset/length + rabitq_offset/length
          + norms_offset/length + address blocks 数据
          + num_blocks(u32) + per-block(base_offset+bit_width+packed)
          + trailer_size(u32) + magic(u32=0x56434C55)
```

**Trailer 设计**：文件末尾 8 字节 = `[trailer_size:u32][magic:u32]`。`ReadInfo` 从文件尾部读 8 字节 → 校验 magic → 读 trailer_size 字节 → 解析完整 ClusterInfo。

### 6.4 Segment

**文件**：`include/vdb/storage/segment.h` + `src/storage/segment.cpp`

```cpp
class Segment {
public:
    explicit Segment(Dim dim, const std::vector<ColumnSchema>& schemas = {});
    
    // 注册 cluster
    Status AddCluster(uint32_t cluster_id,
                      const std::string& clu_path,
                      const std::string& dat_path);
    
    // 获取读取器（懒加载 + 缓存）
    StatusOr<ClusterStoreReader*> GetClusterReader(uint32_t cluster_id);
    StatusOr<DataFileReader*> GetDataFileReader(uint32_t cluster_id);
    
    uint32_t NumClusters() const;
    bool HasCluster(uint32_t cluster_id) const;
};
```

**缓存策略**：首次 `GetClusterReader(id)` 时打开文件并缓存 `unique_ptr`，后续直接返回指针。

---

## 7. Phase 6：IVF 索引（进行中）

### 7.1 已完成：Step 1 — ClusterStoreReader::ReadInfo

在 `ClusterStoreReader` 中新增了 `ReadInfo` 静态方法：

```cpp
static Status ReadInfo(const std::string& path,
                       ClusterStoreWriter::ClusterInfo* out);
```

- 从 `.clu` 文件尾部读取序列化 trailer
- 重建完整的 `ClusterInfo`（含 address_blocks 的 packed 数据）
- 已通过 4 个新增测试（空 cluster、带向量数据、完整往返、错误处理）

### 7.2 未完成：Step 2 — ConANN 三分类

**计划文件**：`include/vdb/index/conann.h` + `src/index/conann.cpp`

```cpp
class ConANN {
    float tau_in_factor_;   // = 1 − ε
    float tau_out_factor_;  // = 1 + ε
    
public:
    static ConANN FromConfig(const RaBitQConfig& cfg, Dim dim);
    // ε = c_factor · 2^(−bits/2) / √dim
    
    ResultClass Classify(float approx_dist, float topk_dist) const;
    // SafeIn  if approx_dist < tau_in * topk_dist
    // SafeOut if approx_dist > tau_out * topk_dist
    // Uncertain otherwise
};
```

**预计测试**：5 cases（epsilon 计算、SafeIn/SafeOut/Uncertain 分类、TopK 未满时行为）

### 7.3 未完成：Step 3 — IvfIndex

**计划文件**：`include/vdb/index/ivf_index.h` + `src/index/ivf_index.cpp`

```cpp
class IvfIndex {
public:
    Status Open(const std::string& dir);
    // 读 segment.meta → centroids.bin → 构造 conann → 注册所有 cluster
    
    std::vector<ClusterID> FindNearestClusters(
        const float* query, uint32_t nprobe) const;
    // 计算 query 到所有 centroid 的 L2 距离
    // std::nth_element 取最小 nprobe 个
    
    const ConANN& conann() const;
    const Segment& segment() const;
    uint32_t nlist() const;
    Dim dim() const;
};
```

**预计测试**：4 cases

### 7.4 未完成：Step 4 — IvfBuilder

**计划文件**：`include/vdb/index/ivf_builder.h` + `src/index/ivf_builder.cpp`

```cpp
struct IvfBuilderConfig {
    uint32_t nlist          = 256;
    uint32_t max_iterations = 50;
    float    tolerance      = 1e-4f;
    uint64_t seed           = 42;
    RaBitQConfig rabitq     = {};
    std::vector<ColumnSchema> payload_schemas = {};
};

class IvfBuilder {
public:
    Status Build(const float* vectors, uint32_t N, Dim dim,
                 const std::vector<std::vector<Datum>>& payloads,
                 const IvfBuilderConfig& config,
                 const std::string& output_dir);
    
private:
    // Phase A：K-means 聚类（随机初始化）
    Status RunKMeans(...);
    
    // Phase B：逐 cluster 写 DataFile + ClusterStore
    Status BuildClusters(...);
    
    // Phase C：写 segment.meta + centroids.bin
    Status WriteSegmentMeta(...);
};
```

**预计测试**：5 cases（小数据集构建、Open 往返、读回向量、空 cluster 处理、带 payload）

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
      Start  1: types_test           ✅
      Start  2: status_test          ✅
      Start  3: aligned_alloc_test   ✅
      Start  4: schema_columns_test  ✅
      Start  5: schema_segment_test  ✅
      Start  6: column_chunk_test    ✅
      Start  7: column_store_test    ✅
      Start  8: test_distance_l2     ✅
      Start  9: test_bit_unpack      ✅
      Start 10: test_prefix_sum      ✅
      Start 11: test_bitpack_codec   ✅
      Start 12: test_dict_codec      ✅
      Start 13: test_popcount        ✅
      Start 14: test_rabitq_rotation ✅
      Start 15: test_rabitq_encoder  ✅
      Start 16: test_rabitq_estimator ✅
      Start 17: test_address_column  ✅
      Start 18: test_data_file       ✅
      Start 19: test_cluster_store   ✅
      Start 20: test_segment         ✅

100% tests passed, 0 tests failed
Total: ~331 individual test cases across 20 executables
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
| `vdb_index`（计划） | index/*.cpp | vdb_storage, vdb_rabitq, vdb_simd, vdb_schema |
| `vdb_query`（计划） | query/*.cpp | vdb_index, vdb_storage, vdb_simd, liburing |

---

## 10. 延迟优化项

以下内容记录在 `UNDO.txt` 中，作为后续可实施的优化：

### Phase 3 延迟项

| ID | 描述 | 优先级 |
|----|------|--------|
| PHASE3-001 | AVX-512 L2 距离路径 | 低 |
| PHASE3-002 | bit_unpack 2/4/8-bit AVX2 特化 | 中 |
| PHASE3-003 | prefix_sum 16/32-wide SIMD 路径 | 低 |
| PHASE3-004 | BitpackCodec::Encode SIMD 写路径 | 低 |

### Phase 4 延迟项

| ID | 描述 | 优先级 |
|----|------|--------|
| PHASE4-001 | 多位 RaBitQ（M=2,4,8）| **高** — 1-bit 精度有限 |
| PHASE4-002 | AVX-512 VPOPCNTDQ 路径 | 低 |
| PHASE4-003 | 编码器内存预分配/对象池 | 中 |
| PHASE4-004 | Hadamard 旋转性能优化（已实现，待 benchmark） | 低 |
| PHASE4-005 | 大维度 Gram-Schmidt 数值稳定性 | 中 |

---

## 11. 关键设计决策记录

| 决策 | 选择 | 理由 |
|------|------|------|
| RaBitQ 算法 | 随机旋转 + 符号量化 | 论文原始方法，非 PQ 标量量化。无偏估计 + 渐近最优误差界 |
| Centroid 参数化 | `centroid=nullptr` → 零向量 | RaBitQ 可独立于 IVF 测试，Phase 6 时使用真实聚类中心 |
| 旋转矩阵 | 随机正交（默认）+ Hadamard（可选） | 随机正交更通用，Hadamard 更快但需 2 的幂维度 |
| 记录存储 | 行存（row-major） | 单条 pread 读取完整记录，适合随机访问 |
| 文件访问 | pread | 线程安全，无文件指针竞争 |
| 页对齐 | 4KB 可配置 | offset/size 存储为页单元，压缩率更高 |
| AddressColumn 解码 | 全块解码（64 条） | SIMD 友好，延迟小（~10 ns/block） |
| ClusterStore 加载 | 全量加载 codes 到内存 | RaBitQ 扫描是热路径，顺序内存访问 >> 随机 I/O |
| Trailer 格式 | 文件末尾 8 字节定位 | ReadInfo 无需外部元数据，自包含 |

---

## 12. 接手开发指南

### 12.1 立即可做的任务

1. **完成 Phase 6 Step 2–4**（ConANN → IvfIndex → IvfBuilder）
   - 参考 `architecture.md` 的 Phase 6 section 和上面 §7 的 API 设计
   - ConANN 最简单（~50 行），建议先做
   - IvfBuilder 最复杂（K-means + 文件写入），但所有底层工具已就绪

2. **Phase 7 查询流水线**
   - 最核心的新组件是 `IoUringReader`（需要 `liburing`）
   - 建议先实现 `PreadFallbackReader` 作为 io_uring 的同步替代
   - `ResultCollector`（TopK heap）可独立实现和测试

### 12.2 代码风格约定

- **命名**：类 `PascalCase`，函数 `PascalCase`，变量 `snake_case`，成员 `trailing_underscore_`
- **文件**：头文件 `.h`，实现 `.cpp`，测试 `_test.cpp`
- **错误处理**：所有 I/O 和解析操作返回 `Status` 或 `StatusOr<T>`
- **SIMD**：`VDB_USE_AVX2` 宏控制编译路径，标量 fallback 必须存在
- **测试**：GoogleTest，每个模块独立 test executable，`ctest` 一键运行全部

### 12.3 已知限制

1. **1-bit RaBitQ 精度**：Spearman 相关性约 0.6-0.8（128 维），对于高精度场景不足。多 bit 扩展（UNDO PHASE4-001）是关键改进方向。
2. **K-means 初始化**：当前计划用随机初始化，可升级为 K-means++。
3. **单线程查询**：Phase 7 初期为单线程协作式调度，多线程扩展需要 MPMC 无锁队列。
4. **无 WAL**：仅支持批量离线构建，不支持在线增量插入。

### 12.4 测试验证流程

```bash
# 修改代码后
cd build
cmake .. && make -j$(nproc) 2>&1 | tail -5   # 编译
ctest --output-on-failure                       # 全量测试

# 单独运行某个测试
./test_rabitq_encoder --gtest_filter="*Encode*"

# 添加新测试后
# 1. 在 tests/<module>/<name>_test.cpp 中编写
# 2. 在 CMakeLists.txt 中添加 add_executable + add_test
# 3. cmake .. && make -j && ctest
```