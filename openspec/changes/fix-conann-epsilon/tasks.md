# Tasks: Fix ConANN Epsilon — Inner-Product Error Bound

## Task 1: IvfBuilder — 替换 ComputeClusterEpsilon 为 ε_ip 标定 + r_max 计算

**Files**: `src/index/ivf_builder.cpp`, `include/vdb/index/ivf_builder.h`

- [ ] 删除 `ComputeClusterEpsilon` 函数
- [ ] 新增 `CalibrateEpsilonIp` 静态函数:
  - 输入: 所有 cluster 的 codes、centroids、rotation、dim、采样参数
  - 对每个 cluster 采样 pseudo-query，计算 `ŝ = 1 - 2·hamming/dim` vs `s = dot(rotated_query, rotated_db)`
  - 收集全局 `|ŝ - s|` pool，返回 P95
- [ ] Build 循环中: 对每个 cluster 计算 `r_max = max(code.norm for code in codes)`
- [ ] `BeginCluster(k, n_members, centroid, r_max)` — 传 r_max 而非重建误差
- [ ] Build 循环后调用 `CalibrateEpsilonIp`，得到全局 `calibrated_eps_ip_`
- [ ] `ConANNParams.epsilon` 写入 `calibrated_eps_ip_`（而非 0.0f）
- [ ] 新增 `float calibrated_eps_ip_ = 0.0f` 成员变量

**注意**: CalibrateEpsilonIp 需要在所有 cluster 编码完成后才能运行（需要全部 codes），因此需要先收集所有 cluster 的 codes 再做标定。可以在 Phase 2 循环中收集，循环结束后统一标定。或者在编码循环中同时计算 per-cluster 的 ip_errors，最后汇总。

**估计**: ~100 行改动

## Task 2: ConANN 类更新

**Files**: `include/vdb/index/conann.h`, `src/index/conann.cpp`

- [ ] 新增 `Classify(float approx_dist, float margin)` 重载:
  ```
  SafeOut: approx_dist > d_k_ + margin
  SafeIn:  approx_dist < d_k_ - margin
  Uncertain: otherwise
  ```
- [ ] 更新类注释: epsilon 语义变为 ε_ip（内积误差上界），margin 在 query 时计算
- [ ] 保留旧 `Classify(float approx_dist)` 向后兼容

**估计**: ~20 行改动

## Task 3: OverlapScheduler — 动态 margin 计算

**Files**: `src/query/overlap_scheduler.cpp`

- [ ] ProbeCluster 中移除 `ConANN conann(pc.epsilon, index_.conann().d_k())`
- [ ] 改为:
  ```
  float r_max = pc.epsilon;  // .clu 字段现在存 r_max
  float eps_ip = index_.conann().epsilon();
  float d_k = index_.conann().d_k();
  float margin = 2.0f * r_max * pq.norm_qc * eps_ip;
  ```
- [ ] 内循环 Classify 调用改为 `index_.conann().Classify(dists[i], margin)` 或直接内联比较

**估计**: ~15 行改动

## Task 4: segment_meta.fbs 注释更新

**Files**: `schema/segment_meta.fbs`

- [ ] `ConANNParams.epsilon` 注释更新为 `ε_ip: 全局内积估计误差上界 P95(|ŝ-s|)`
- [ ] 移除旧的 `c·2^(−B/2)/√D` 注释
- [ ] 更新 `RaBitQParams.c_factor` 注释标注为 deprecated（不再参与 epsilon 计算）

**估计**: ~5 行改动

## Task 5: 更新测试

**Files**: `tests/storage/cluster_store_test.cpp`, `tests/query/overlap_scheduler_test.cpp`, `tests/query/early_stop_test.cpp`, `tests/index/ivf_index_test.cpp`

- [ ] `cluster_store_test`: epsilon 字段值改为 r_max 值测试
- [ ] `overlap_scheduler_test`: 适配新的 margin 机制
- [ ] `early_stop_test`: 适配新的 margin 机制
- [ ] `ivf_index_test`: ConANN 加载验证 — epsilon 应为 ε_ip > 0

**估计**: ~30 行改动

## Task 6: bench_rabitq_accuracy 更新

**Files**: `tests/index/bench_rabitq_accuracy.cpp`

- [x] Phase 2b 改为: 计算 per-cluster r_max 和全局 ε_ip 标定
- [x] 输出 ε_ip 值和 per-cluster r_max 统计表
- [x] Phase 4 分类改为: `margin = 2 · r_max · r_q · ε_ip`，使用 `Classify(dist, margin)`
- [x] 移除 `c_factor` 硬编码

**估计**: ~40 行改动

## Task 7: 编译验证 + 运行 benchmark

- [x] 编译全部 target 无 warning
- [x] 运行所有 test 通过 (35/35)
- [x] 运行 `bench_rabitq_accuracy --dataset /home/zcq/VDB/data/coco_1k`
- [x] 验证: ε_ip = 0.072 (dim=512), SafeOut = 100%, false SafeOut = 1.0%
