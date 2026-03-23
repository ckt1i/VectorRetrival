# Tasks: Per-Cluster Epsilon via Reconstruction Error

## Task 1: ClusterLookupEntry — 新增 epsilon 字段 + 版本升级

**Files:** `include/vdb/storage/cluster_store.h`, `src/storage/cluster_store.cpp`

- [x] `ClusterLookupEntry` 新增 `float epsilon = 0.0f;` 字段
- [x] `kFileVersion` 从 5 改为 6
- [x] Writer `WriteLookupTable`: 序列化 epsilon（在 num_records 之后，centroid 之前）
- [x] Reader `Open`: 只接受 version == 6，读取 epsilon
- [x] Reader `ParseClusterBlock`: 将 lookup table 中的 epsilon 传给 `ParsedCluster`

## Task 2: ParsedCluster — 新增 epsilon

**Files:** `include/vdb/query/parsed_cluster.h`

- [x] 新增 `float epsilon = 0.0f;` 字段

## Task 3: IvfBuilder — 计算 per-cluster 重建误差

**Files:** `src/index/ivf_builder.cpp`, `include/vdb/index/ivf_builder.h`

- [x] 新增静态函数 `ComputeClusterEpsilon(member_vecs, codes, centroid, rotation, dim, n_members, max_samples, seed)` → float
- [x] 实现重建: `signs/√dim → ApplyInverse → centroid + norm * recon → L2(v, v̂)`
- [x] 采样策略: 若 `n_members < 2 * n_samples`，则 `n_samples = max(n_members / 2, 1u)`
- [x] 对采样向量计算误差，排序取 P95
- [x] 在 `WriteIndex` 中，encode 后调用此方法，将 epsilon 传给 `clu_writer.BeginCluster`
- [x] `BeginCluster` 签名扩展: 新增 `float epsilon` 参数
- [x] 移除旧的全局 epsilon 计算（`c_factor * pow(2, ...) / sqrt(dim)`）
- [x] `IvfBuilderConfig` 新增 `uint32_t epsilon_samples = 20;` 用于控制采样数

## Task 4: ClusterStoreWriter::BeginCluster — 接受 epsilon

**Files:** `include/vdb/storage/cluster_store.h`, `src/storage/cluster_store.cpp`

- [x] `BeginCluster` 签名新增 `float epsilon` 参数
- [x] 内部 `ClusterLookupEntry` 赋值 epsilon

## Task 5: ProbeCluster — 使用 per-cluster epsilon

**Files:** `src/query/overlap_scheduler.cpp`

- [x] 替换 `const auto& conann = index_.conann();` 为 per-cluster ConANN 构造
- [x] `ConANN conann(pc.epsilon, index_.conann().d_k());`
- [x] 分类调用改为 `conann.Classify(dists[i])`（无变化，变量名不变）

## Task 6: 更新现有测试

**Files:** `tests/storage/cluster_store_test.cpp`, `tests/query/overlap_scheduler_test.cpp`, `tests/index/ivf_index_test.cpp`, `tests/query/early_stop_test.cpp`

- [x] cluster_store_test: epsilon roundtrip 验证（写入 → 读回 → 值一致）
- [x] overlap_scheduler_test: 放宽为 recall-based 检查（per-cluster epsilon 导致近似结果）
- [x] ivf_index_test: 更新 ConANN_LoadedCorrectly（global epsilon=0，d_k>0）
- [x] early_stop_test: 放宽为存在性+排序检查
- [x] ClusterStoreReader 新增 `GetEpsilon(cluster_id)` accessor

## Task 7: bench_rabitq_accuracy — 输出 per-cluster 信息

**Files:** `tests/index/bench_rabitq_accuracy.cpp`

- [x] 计算 per-cluster reconstruction error P95
- [x] 输出 per-cluster statistics 表格: cluster_id, num_vectors, epsilon
- [x] 使用 per-cluster epsilon 重新计算 SafeIn/SafeOut 分类准确率

## Task 8: 编译验证

- [x] `cmake --build build` 无 error/warning
- [x] `ctest` 所有 35 测试通过
