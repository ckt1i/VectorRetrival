# Tasks: Norm Storage & Zero-Copy ProbeCluster

## Task 1: ClusterStoreWriter — 写入 norm + sum_x

**Files:** `src/storage/cluster_store.cpp`, `include/vdb/storage/cluster_store.h`

- [x] Global header version 常量从 4 改为 5
- [x] `WriteVectors`: 每条 code 后追加写入 `code.norm` (float32) 和 `code.sum_x` (uint32)
- [x] 更新 code_entry_size 计算: `num_words * 8 + 8`
- [x] 更新已有 cluster_store_test 验证新格式

## Task 2: ClusterStoreReader — 解析新格式

**Files:** `src/storage/cluster_store.cpp`

- [x] `ParseBlock`: code_entry_size 使用 `num_words * 8 + 8` (只接受 v5)
- [x] ParsedCluster.code_entry_size 正确反映新格式
- [x] 验证 codes_start 指针和 address blocks 偏移正确

## Task 3: RaBitQEstimator — 零拷贝接口

**Files:** `include/vdb/rabitq/rabitq_estimator.h`, `src/rabitq/rabitq_estimator.cpp`

- [x] 新增 `EstimateDistanceRaw(pq, code_words, num_words, norm_oc)` 方法
- [x] 实现: PopcountXor → ip_est → dist_sq (与 EstimateDistance 逻辑一致)
- [x] 单元测试: 验证 EstimateDistanceRaw 与 EstimateDistance 输出一致

## Task 4: ProbeCluster 零拷贝重写

**Files:** `src/query/overlap_scheduler.cpp`

- [x] 删除 `std::vector<RaBitQCode> batch_codes` 的分配
- [x] 直接从 `pc.codes_start + idx * pc.code_entry_size` 读取 code_ptr / norm
- [x] 调用 `EstimateDistanceRaw` 替代 `EstimateDistanceBatch`
- [x] 删除 `PopcountTotal` 调用 (不再需要 sum_x)
- [ ] 验证 overlap_scheduler_test 通过 (blocked: 需要先通过 Task 5 确定 c_factor)

## Task 5: RaBitQ 精度 Benchmark

**Files:** `tests/index/bench_rabitq_accuracy.cpp`, `CMakeLists.txt`

- [x] 加载 coco_1k 数据集 (image_embeddings.npy, query_embeddings.npy)
- [x] K-Means 分 cluster，对每个向量编码 RaBitQ code
- [x] 对 1000 queries × 1000 images 计算 exact L2² 和 RaBitQ 估计距离
- [x] 统计距离偏差: mean/max 绝对误差, mean/p95/p99 相对误差
- [x] 统计排序准确率: recall@1, recall@5, recall@10
- [x] 统计 SafeIn/SafeOut 分类准确率 (以 exact top-K 为 GT):
  - SafeIn correct rate, SafeOut correct rate
  - False SafeIn rate, False SafeOut rate
  - Uncertain %
- [x] 添加 CMake target `bench_rabitq_accuracy`
