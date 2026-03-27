# Tasks: CRC OverlapScheduler Integration

## 1. ConANN ClassifyAdaptive

- [ ] 1.1 `conann.h` 新增 `ClassifyAdaptive(float approx_dist, float margin, float dynamic_d_k) const` 声明
- [ ] 1.2 `conann.cpp` 实现 ClassifyAdaptive: SafeOut 用 dynamic_d_k + 2*margin, SafeIn 用静态 d_k_ - 2*margin
- [ ] 1.3 单元测试: 验证 dynamic_d_k < d_k 时 SafeOut 增多, SafeIn 不变

## 2. CrcCalibrator RaBitQ 距离改造

- [ ] 2.1 `crc_calibrator.h` 扩展 ClusterData: 新增 codes_block, code_entry_size 字段
- [ ] 2.2 `crc_calibrator.h` Calibrate 接口新增 `const RotationMatrix& rotation` 参数
- [ ] 2.3 `crc_calibrator.cpp` 改造 ComputeScoresForQuery: 用 RaBitQEstimator 替换 simd::L2Sqr
  - 每个 cluster 做 PrepareQuery(query, centroid, rotation)
  - 从 codes_block 读取 code_words + norm_oc
  - 调用 EstimateDistanceRaw 得到估计距离
- [ ] 2.4 更新 CrcCalibrator 单元测试: 提供 RaBitQ codes 输入
- [ ] 2.5 验证标定结果 d_min/d_max 在 RaBitQ 距离空间合理

## 3. OverlapScheduler 集成

- [ ] 3.1 `search_context.h` SearchConfig 新增字段:
  - `const CalibrationResults* crc_params = nullptr`
  - `uint32_t initial_prefetch = 4`
  - `probe_batch_size` 默认值改为 128
- [ ] 3.2 `search_context.h` SearchStats 新增:
  - `uint32_t crc_clusters_probed = 0`（CRC 视角的 probed count）
- [ ] 3.3 `overlap_scheduler.h` 新增成员:
  - `CrcStopper crc_stopper_`
  - `std::vector<std::pair<float, uint32_t>> est_heap_`
  - `uint32_t est_top_k_`
  - `bool use_crc_`（= crc_params != nullptr）
- [ ] 3.4 OverlapScheduler 构造函数: 当 crc_params 有效时初始化 CrcStopper
- [ ] 3.5 Search() 入口: 每次查询前 clear est_heap_
- [ ] 3.6 ProbeCluster 改造:
  - 进入时读取 dynamic_d_k (est_heap_.top 或 static d_k fallback)
  - 分类调用改为 ClassifyAdaptive(dist, margin, dynamic_d_k)（use_crc_=true 时）
  - 每个 vector 的 RaBitQ dist 同时更新 est_heap_
  - 仅 use_crc_=false 时使用原 Classify
- [ ] 3.7 早停替换 (ProbeAndDrainInterleaved L258-265):
  - use_crc_=true: crc_stopper_.ShouldStop(i+1, est_heap_.top)
  - use_crc_=false: 保持原 d_k 逻辑
- [ ] 3.8 PrefetchClusters: 使用 config_.initial_prefetch 替代 config_.prefetch_depth 作为初始提交数
- [ ] 3.9 编译验证: 确保 crc_params=nullptr 时行为与改动前完全一致

## 4. Benchmark: SafeOut 误杀率对比

- [ ] 4.1 新增或修改 benchmark: 对同一查询集运行两种 Classify 策略
  - 静态: Classify(dist, margin) with d_k
  - 动态: ClassifyAdaptive(dist, margin, dynamic_d_k)
- [ ] 4.2 计算并输出: SafeOut count/误杀率、SafeIn count、Uncertain count（旧 vs 新）
- [ ] 4.3 误杀率计算: 用精确 L2 ground truth 检查被 SafeOut 的 vector 是否在真实 top-k 中

## 5. Benchmark: 端到端性能对比

- [ ] 5.1 构建端到端 benchmark: 加载真实索引 → 运行查询 → 收集 SearchStats
- [ ] 5.2 对比配置:
  - baseline: d_k 早停 + 静态 SafeOut + prefetch=16
  - CRC-only: CRC 早停 + 静态 SafeOut + initial_prefetch=4
  - CRC+dynamic: CRC 早停 + 动态 SafeOut + initial_prefetch=4
- [ ] 5.3 输出指标: total_time_ms, probe_time_ms, io_wait_time_ms, rerank_time_ms
- [ ] 5.4 计算 overlap_ratio = 1 - io_wait_time / total_time

## 6. Benchmark: Prefetch 敏感性

- [ ] 6.1 对比 initial_prefetch = {4, 8, 16} 下的搜索时间和 overlap_ratio
- [ ] 6.2 输出 recall@1/5/10, avg_probed, early_stop_rate

## 7. 现有测试验证

- [ ] 7.1 确保现有 CrcStopper 单元测试全部通过
- [ ] 7.2 确保现有 CrcCalibrator 单元测试更新后通过（适配新接口）
- [ ] 7.3 确保 OverlapScheduler 现有测试（crc_params=nullptr）通过
- [ ] 7.4 运行 bench_e2e 验证端到端 recall 不退化
