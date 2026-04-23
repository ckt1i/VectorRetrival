## 1. Build Subset-Level Static Cache

- [x] 1.1 梳理当前 `BuildStopProfiles`、`RegularizeScores` 和 `Calibrate` 的数据依赖，抽出与 `reg_lambda` 无关的 subset-level 静态统计边界
- [x] 1.2 为固定 subset 实现静态 cache，至少覆盖 `gt_size` 与原始 `step` 索引下的 `overlap_by_step`
- [x] 1.3 让 calib / tune / test 的 profile 构建复用静态 cache，而不是在每次 `reg_lambda` 或 profile build 时重算 overlap

## 2. Precompute Rank For Regularization

- [x] 2.1 为固定 subset 的 `nonconf` 实现一次性 rank 预计算，生成 `rank_by_step`
- [x] 2.2 将 `RegularizeScores` 或等价路径改为复用预计算 rank，仅保留基于 `reg_lambda` 和 `kreg` 的公式展开
- [x] 2.3 验证 rank 预计算路径与当前 `RegularizeScores` 在 reg-score 排序和 stop-step 选择上保持等价

## 3. Tighten Profile Construction

- [x] 3.1 重构 `BuildStopProfiles`，使其只负责基于 `reg_scores` 的排序结果投影静态 cache，而不再承担完整 overlap 统计计算
- [x] 3.2 收敛 profile 构建中的临时容器与分配成本，减少 `new/free` 和小对象开销
- [x] 3.3 保持当前 profile-based Brent 契约、`lamhat` 语义和 `CalibrationResults` 输出不变

## 4. Validate Behavior And Perf

- [x] 4.1 在固定 `crc_scores.bin`、固定 split/seed 下，对比优化前后的 `lamhat`、`actual_fnr`、`avg_probed` 和 `recall@10`
- [x] 4.2 在固定 `bench_e2e` CRC 运行点下，对比优化前后的在线 `recall@10`、`avg_probed_clusters`、`early_stop` 与 `avg_skipped`
- [x] 4.3 复跑 runtime CRC perf，确认 preparation 热点从重复 overlap / rank 排序中退出，并记录剩余 Brent / sorting 成本是否足以支撑下一步离散候选阈值搜索 proposal
