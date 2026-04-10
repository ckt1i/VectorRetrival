## Context

两个串行校准循环是 Phase B 在完成 encoder 和 eps 优化后的新最大瓶颈：

- `CalibrateDistanceThreshold`：100 个样本 × 100 万次 L2Sqr + nth_element（串行）
- `ComputeAllScores[RaBitQ]`：N 个查询 × 逐集群探测（串行）

两个循环的外层迭代完全独立，属于 embarrassingly parallel。

## Decisions

### Decision 1: d_k 并行化策略

**问题**：`dists` 缓冲区（大小 N=1M）当前在循环外分配（`std::vector<float> dists(N)`），多线程共享会导致数据竞争。

**选项：**
- Option A：每次迭代局部分配 `std::vector<float> dists(N)`（栈上 new/delete，~4MB per call）
- Option B：预分配 `std::vector<std::vector<float>> tl_dists(nt)` per-thread 缓冲区

**选择 Option B**：避免循环内热路径上的 malloc（N=1M 浮点，每次 4MB 分配）。与 eps-calibration-simd 的 thread-local vector 策略一致。

**实现：**
```cpp
// 在循环外：
#ifdef _OPENMP
const int nt = omp_get_max_threads();
#else
const int nt = 1;
#endif
std::vector<std::vector<float>> tl_dists(nt,
    std::vector<float>(N));  // 预分配，一次性

#pragma omp parallel for schedule(dynamic, 4)
for (int s = 0; s < static_cast<int>(num_samples); ++s) {
#ifdef _OPENMP
    int tid = omp_get_thread_num();
#else
    int tid = 0;
#endif
    auto& dists = tl_dists[tid];
    // ... 原逻辑不变，写 sample_dk[s]（不同 index，安全）
}
// sort + percentile 不变
```

**注意**：`schedule(dynamic, 4)` 代替 `schedule(static)`，因为 num_samples 只有 100，chunk=4 足够均衡。

### Decision 2: CRC 并行化策略

**问题**：`ComputeAllScores` 和 `ComputeAllScoresRaBitQ` 各自有一个独立查询的循环。

**分析**：
- `all_scores` 预分配为 `query_indices.size()`
- `all_scores[i]` 每个索引由一个线程写入，无竞争
- `ComputeScoresForQuery` 内部分配所有临时数据（`centroid_dists`、`heap`、`sorted_heap`），无全局共享状态
- `ComputeScoresForQueryRaBitQ` 同上，额外用 `PrepareQuery`（thread-safe，per-call 分配）

**解决方案**：直接加 `#pragma omp parallel for schedule(dynamic)`，无需额外重构。

```cpp
// ComputeAllScores 改动：
#pragma omp parallel for schedule(dynamic)
for (int i = 0; i < static_cast<int>(query_indices.size()); ++i) {
    uint32_t qi = query_indices[i];
    const float* q = queries + static_cast<size_t>(qi) * dim;
    all_scores[i] = ComputeScoresForQuery(q, dim, centroids, nlist, clusters, top_k);
}
```

（`ComputeAllScoresRaBitQ` 完全相同的改动）

### Decision 3: OpenMP 头文件引入

`crc_calibrator.cpp` 当前不包含 `<omp.h>`。

**方案**：与 `bench_vector_search.cpp` 一致，使用 `#ifdef _OPENMP` 守卫：
```cpp
#ifdef _OPENMP
#include <omp.h>
#endif
```

`conann.cpp` 同上。

CMake 方面：`vdb_index` 库需要链接 `OpenMP::OpenMP_CXX`（条件）。查当前 CMakeLists.txt `find_package(OpenMP)` 已存在（eps-calibration-simd 加入），只需对 `vdb_index` 添加链接。

### Decision 4: 线程安全验证

**d_k (`CalibrateDistanceThreshold`)**：
- `tl_dists[tid]`：per-thread，安全
- `sample_dk[s]`：每个 `s` 由唯一一个线程写入（loop index），安全
- `indices`（query 索引数组）：只读，安全
- `queries`, `database`：只读，安全
- `rng`：在循环前 shuffle，循环内不使用 rng，安全

**CRC (`ComputeScoresForQuery`)**：
- `centroid_dists`：per-call 局部变量（`std::vector<>`），安全
- `heap`：per-call 局部变量，安全
- `clusters[]`：只读，安全
- `centroids`：只读，安全
- `all_scores[i]`：不同线程写不同索引，安全

**CRC RaBitQ (`ComputeScoresForQueryRaBitQ`)**：
- `PrepareQuery`：per-call 返回 `PreparedQuery`，无共享状态（已在 eps-calibration-simd 中验证）
- 其余同上

**结论**：两处改动均不需要 mutex 或 atomic。

### Decision 5: CMake 集成

当前 `vdb_index` 的链接：
```cmake
target_link_libraries(vdb_index PUBLIC vdb_storage vdb_rabitq vdb_simd vdb_schema vdb_io superkmeans)
```

需要在现有 `find_package(OpenMP)` 后添加条件链接：
```cmake
if(OpenMP_CXX_FOUND)
    target_link_libraries(vdb_index PRIVATE OpenMP::OpenMP_CXX)
endif()
```

### Decision 6: 验证策略

- 正确性：`eps_ip`、`eps_ip_fs`、`lamhat`、`recall@10` 数值不变（P-percentile 不依赖排序顺序）
- d_k：`d_k` 值本身也是 P-percentile，与采样顺序无关，应 bit-exact（因为 `sample_dk[s]` 按索引写入）
- 性能：记录 Phase B 总耗时变化

## Risks / Trade-offs

- **[内存开销]** d_k per-thread 缓冲：`nt × N × 4B` = 8 × 1M × 4 = 32 MB 额外内存。可接受（系统已用数 GB）。
- **[CRC heap 分配热点]** `ComputeScoresForQuery` 内部多次 `std::vector` 分配（centroid_dists N 大小，heap 动态增长）。每次调用约分配 nlist×sizeof(pair<float,uint32>) = 4096×8 = 32KB。并行下 allocator 压力增加，预计 3-5× 加速（非理想 8×）。
- **[调度粒度]** d_k 的 100 个样本用 `schedule(dynamic, 4)`；CRC 的查询数量通常在 100-500 之间，`schedule(dynamic)` 足够。
- **[determinism]** d_k 结果 bit-exact（sample_dk[s] 按固定 index 写）；CRC 校准涉及数值聚合，可能有浮点舍入顺序差异，但最终统计量（lamhat、d_min、d_max）误差 < 1e-5。
