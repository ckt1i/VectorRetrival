Research Phase 6 prerequisites
Searched for files matching `**/include/vdb/storage/cluster_store.h`, 1 match

Now I have enough context. Let me draft the plan. 

Asked 3 questions (K-means init, 持久化格式, ConANN ε)

现在我有足够的信息了。以下是完整的实现计划：

---

## Plan: Phase 6 — IVF Index

基于已完成的 Phase 0–5，在 `include/vdb/index/` + `src/index/` 实现三个组件：`ConANN` 三分类、`IvfIndex` 查询入口、`IvfBuilder` 离线构建器。元数据持久化使用现有 `SegmentMeta` FlatBuffers schema，K-means 采用随机初始化，ConANN 的 ε 从 `ConANNParams` 读取。

---

**Steps**

**Step 1 — 扩展 ClusterStoreReader（修改 cluster_store.h + cluster_store.cpp）**

新增一个静态方法，用于 IvfIndex::Open 重建 ClusterInfo 时不需要外部传入 info：
```
static Status ReadInfo(path, Dim dim, RaBitQConfig, ClusterStoreWriter::ClusterInfo* out)
```
该方法打开 .clu 文件，读取其内部 FlatBuffers 头，重建 ClusterInfo（cluster_id、num_records、各段 offset/length、address_blocks）。这样 IvfIndex::Open 只需知道每个 cluster 的 .clu 路径即可完整重建索引结构。

---

**Step 2 — 实现 `ConANN`（新增 include/vdb/index/conann.h + src/index/conann.cpp）**

```
class ConANN {
    float tau_in_factor_;   // = 1 − ε
    float tau_out_factor_;  // = 1 + ε，ε = c·2^(−B/2)/√D

public:
    ConANN(float tau_in_factor, float tau_out_factor)
    static ConANN FromConfig(const RaBitQConfig& cfg, Dim dim)
        // 计算 ε = cfg.c_factor * pow(2, -cfg.bits/2.0f) / sqrt(dim)
        // tau_in = 1 − ε,  tau_out = 1 + ε
    ResultClass Classify(float approx_dist, float topk_dist) const
        // SafeIn  if approx_dist < tau_in_factor_  * topk_dist
        // SafeOut if approx_dist > tau_out_factor_ * topk_dist
        // Uncertain otherwise
    float epsilon() const
    float tau_in_factor() const
    float tau_out_factor() const
}
```

注：分类在距离域（非平方），与 RaBitQEstimator 的输出单位一致。

---

**Step 3 — 实现 `IvfIndex`（新增 include/vdb/index/ivf_index.h + src/index/ivf_index.cpp）**

```
class IvfIndex {
    Dim dim_;
    uint32_t nlist_;
    std::vector<float>      centroids_;   // row-major, nlist × dim
    std::vector<ClusterID>  cluster_ids_; // ordered list
    ConANN                  conann_;
    Segment                 segment_;

public:
    Status Open(const std::string& dir)
        // 1. 读 dir/segment.meta (FlatBuffers SegmentMeta)
        // 2. 从 IvfParams 加载 nlist、dim、读 centroids.bin → centroids_
        // 3. 从 ConANNParams 加载 tau_in_factor/tau_out_factor → 构造 conann_
        // 4. 遍历每个 ClusterMeta → ClusterStoreReader::ReadInfo(clu_path,...) → segment_.AddCluster
    
    // 查询时使用
    std::vector<ClusterID> FindNearestClusters(const float* query, uint32_t nprobe) const
        // 对所有 nlist 个 centroid 计算 simd::L2Sqr
        // std::nth_element 取最小 nprobe 个 → 返回对应 cluster_ids
    
    const ConANN& conann() const
    const Segment& segment() const
    uint32_t nlist() const;  Dim dim() const
}
```

`FindNearestClusters` 时间复杂度 O(nlist × D)，使用 simd::L2Sqr 加速。

---

**Step 4 — 实现 `IvfBuilder`（新增 include/vdb/index/ivf_builder.h + src/index/ivf_builder.cpp）**

```
struct IvfBuilderConfig {
    uint32_t nlist          = 256;
    uint32_t max_iterations = 50;
    float    tolerance      = 1e-4f;  // centroid 变化停止阈值
    uint64_t seed           = 42;
    RaBitQConfig rabitq     = {};
    std::vector<ColumnSchema> payload_schemas = {};
};

class IvfBuilder {
public:
    // 主接口：输入 N 个 dim 维向量 + 可选 payload，输出写入到 output_dir
    Status Build(
        const float*                        vectors,       // N × dim row-major
        uint32_t                            N,
        Dim                                 dim,
        const std::vector<std::vector<Datum>>& payloads,   // N 条，可为空
        const IvfBuilderConfig&             config,
        const std::string&                  output_dir);

private:
    // Phase A — K-means（随机初始化）
    // 1. 随机选 nlist 个向量作初始 centroid（mt19937 seed）
    // 2. 迭代 max_iterations 次：
    //    a. Assign: 每个向量找最近 centroid（L2Sqr，O(N·nlist·D)）
    //    b. Update: 重算每个 cluster 的均值 centroid
    //    c. 若所有 centroid 移动总量 < tolerance → 提前终止
    // 返回：centroids[nlist×dim]，assignments[N]
    Status RunKMeans(const float* vectors, uint32_t N, Dim dim,
                     const IvfBuilderConfig& config,
                     std::vector<float>& centroids,
                     std::vector<uint32_t>& assignments);

    // Phase B — 逐 cluster 写 DataFile + ClusterStore
    // 对每个 cluster c（仅处理非空 cluster）：
    //   DataFileWriter: Open("cluster_CCCC.dat", c, dim, payload_schemas, page_size=1)
    //   for each vector i in cluster c:
    //     RaBitQEncoder::Encode(vec, centroid[c]) → RaBitQCode
    //     DataFileWriter::WriteRecord(vec, payloads[i], addr)
    //   DataFileWriter::Finalize() → addr_entries[]
    //   AddressColumn::Encode(addr_entries, 64, 1) → addr_blocks
    //   ClusterStoreWriter: Open/WriteCentroid/WriteVectors/WriteAddressBlocks/Finalize
    // 返回：per-cluster ClusterStoreWriter::ClusterInfo[]
    Status BuildClusters(...);

    // Phase C — 写持久化元数据
    // 1. centroids.bin：裸 float binary，nlist × dim × sizeof(float)
    // 2. segment.meta：
    //    FlatBuffers SegmentMeta {
    //      ivf_params: IvfParams {
    //        nlist, nprobe=config.nlist, centroids_offset=0,
    //        centroids_length=nlist*dim*4 }
    //      conann_params: ConANNParams {
    //        tau_in_factor  = 1 − ε,
    //        tau_out_factor = 1 + ε,
    //        ε = c·2^(−B/2)/√D }
    //      clusters: [ClusterMeta per cluster]
    //    }
    Status WriteSegmentMeta(
        const std::string& dir, Dim dim,
        const IvfBuilderConfig& config,
        const std::vector<float>& centroids,
        const std::vector<ClusterStoreWriter::ClusterInfo>& cluster_infos);
};
```

ClusterMeta 的 AddressColumnMeta 字段由 ClusterInfo.address_blocks 序列化而来（base_offset、bit_width、packed data 写入 ClusterMeta 的 FlatBuffer，或记录已在 .clu 文件中的 data_offset/length，取决于 Step 1 中 ReadInfo 的实现）。

---

**Step 5 — 更新 CMakeLists.txt（修改 CMakeLists.txt）**

```cmake
# 新建目录 src/index/
add_library(vdb_index STATIC
    src/index/conann.cpp
    src/index/ivf_index.cpp
    src/index/ivf_builder.cpp)
target_include_directories(vdb_index PUBLIC include)
target_link_libraries(vdb_index PUBLIC
    vdb_storage      # Segment, DataFileWriter/Reader, ClusterStore, AddressColumn
    vdb_rabitq       # RaBitQEncoder, RaBitQEstimator, RotationMatrix
    vdb_simd         # L2Sqr
    vdb_schema)      # 生成的 FlatBuffers 头（SegmentMeta 等）

# 测试
add_executable(test_conann   tests/index/conann_test.cpp)
add_executable(test_ivf_index tests/index/ivf_index_test.cpp)
add_executable(test_ivf_builder tests/index/ivf_builder_test.cpp)
foreach(t test_conann test_ivf_index test_ivf_builder)
    target_link_libraries(${t} PRIVATE vdb_index GTest::gtest GTest::gtest_main)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
```

---

**Step 6 — 测试用例 tests/index/**

**`conann_test.cpp`**：
- `FromConfig_ComputesCorrectEpsilon`：给定 c=5.75, bits=1, dim=64，验证 epsilon = 5.75/8 ≈ 0.719，tau_in_factor ≈ 0.281，tau_out_factor ≈ 1.719
- `Classify_SafeIn`：approx_dist=0.5, topk=1.0, epsilon=0.3 → SafeIn（0.5 < 0.7×1.0）
- `Classify_SafeOut`：approx_dist=1.5 > 1.3 → SafeOut
- `Classify_Uncertain`：approx_dist=0.9 in [0.7, 1.3] → Uncertain
- `Classify_TopKNotYetFull`：topk_dist=inf → 所有 candidate 为 Uncertain（避免误判 SafeOut）

**`ivf_index_test.cpp`**：
- `FindNearestClusters_ReturnsCorrectCount`：构建 8 cluster index，nprobe=3 → 返回 3 个不重复 cluster_ids
- `FindNearestClusters_NearestIsFirst`：query=centroid[0] → cluster_ids[0]=0
- `Open_InvalidDir`：Open 一个不存在目录 → Status not ok
- `GetConANN_IsValid`：conann().tau_in_factor < 1.0

**`ivf_builder_test.cpp`**：
- `Build_SmallDataset`：100 个 dim=16 向量，nlist=4 → 验证生成 4 组 .clu/.dat 文件，segment.meta、centroids.bin 均存在
- `Build_OpenRoundTrip`：Build 后立即 Open → num_clusters == nlist, FindNearestClusters 返回正确数量
- `Build_ReadBackVectors`：Build 后 Open，对每个 cluster 用 Segment::GetDataFile 读回向量 → 与原始向量匹配
- `Build_EmptyCluster_Handled`：向量数量 < nlist → 部分 cluster 为空，不崩溃，num_clusters <= nlist
- `Build_WithPayload`：带 payload_schemas 构建 → ReadRecord 返回正确 payload

---

**Verification**

```bash
cd build && cmake .. && make -j$(nproc)
ctest -R "test_conann|test_ivf_index|test_ivf_builder" --output-on-failure
```

目标：新增 3 个测试可执行文件全部通过，现有 20 个测试保持通过。

---

**Decisions**

- **ClusterStoreReader 扩展**：新增 `ReadInfo` 静态方法是最小侵入方案，不改变现有接口语义
- **K-means 初始化**：随机（seed 固定），Phase 6 后可升级为 K-means++
- **ConANN epsilon**：IvfBuilder 构建时计算、持久化到 `ConANNParams.tau_in_factor/tau_out_factor`；IvfIndex::Open 直接读取，无需运行时重算
- **持久化格式**：现有 `SegmentMeta` FlatBuffers schema，`centroids.bin` 作独立裸 float 文件，`IvfParams.centroids_offset=0`
- **空 cluster 处理**：仅写有至少 1 条记录的 cluster，SegmentMeta 中 clusters 列表可少于 nlist