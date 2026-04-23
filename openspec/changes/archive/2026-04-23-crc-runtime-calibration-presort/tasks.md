## 1. Refactor CRC Calibration Evaluation Data

- [x] 1.1 梳理 `src/index/crc_calibrator.cpp` 中 `RegularizeScores`、`ComputePredictions`、`FalseNegativeRate` 和 `PickLambdaReg` 的当前依赖关系，并抽出 query-level stop profile 所需的最小数据结构
- [x] 1.2 为每条 calibration query 实现一次性 `(reg_score, step)` 预排序的数据准备路径，确保后续 `lambda` 评估可直接复用
- [x] 1.3 为 stop profile 增加排序位置到原始 step 的映射和等价 probe 计数，保持与旧 stop 规则一致

## 2. Replace Repeated Work Inside Brent Evaluation

- [x] 2.1 将 `ComputePredictions` 或等价评估路径改为基于 stop profile 的二分查询，而不是每次 `lambda` 评估重新排序
- [x] 2.2 在 calibration 预处理阶段增加 overlap / hit-count 预计算或等价查表结构，避免 Brent 迭代中重复重建 prediction 集
- [x] 2.3 更新 `FalseNegativeRate`、`ComputeRecall` 或等价统计逻辑，使其使用 stop profile + 查表结果并保持现有 Brent solver 和 `CalibrationResults` 语义不变

## 3. Validate Semantic Parity

- [x] 3.1 在固定 `crc_scores.bin` 和固定 split/seed 下，对比优化前后的 `lamhat`、`actual_fnr`、`avg_probed` 和 `recall@10`
- [x] 3.2 在 `bench_e2e` CRC 路径上验证优化前后线上 `recall@10`、`avg_probed_clusters`、`early_stop` 行为保持一致或仅有可解释的数值级偏差
- [x] 3.3 补充必要日志或断言，确保 profile-based 路径没有悄悄切换到离散候选阈值搜索

## 4. Measure Performance Impact

- [x] 4.1 复跑 runtime CRC benchmark preparation / query-only perf，对比 `CrcCalibrator::Calibrate` 总耗时和 `ComputePredictions + sort` 热点占比
- [x] 4.2 记录本次 change 的收益边界和剩余热点，明确是否需要单独开启下一步离散候选阈值搜索 proposal
