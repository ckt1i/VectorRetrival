## Why

`crc-runtime-calibration-presort` 已经把 `ComputePredictions + sort` 从 CRC preparation 主热点中移除，但最新 perf 显示新的主要成本已经收敛到 `BuildStopProfiles`。这说明当前 preparation 阶段仍然存在两类明显重复工作：每个 `reg_lambda` 下重复计算 step-level overlap，以及 `RegularizeScores` 中对同一 query 的 `nonconf` 反复做 rank 排序。

因此下一步优化不应直接切换 solver，而应继续在保持 Brent 语义不变的前提下，把 CRC runtime calibration 的剩余重复工作拆成可复用 cache，并收敛 profile 构建的分配与布局成本。这样可以进一步降低 preparation 延迟，并为之后是否引入离散候选阈值搜索提供更干净的性能基线。

## What Changes

- 为 calibration subset 引入静态 cache，预先保存与 `reg_lambda` 无关的 step-level 统计，例如 `gt_size` 和 `overlap_by_step`，避免在每个 `reg_lambda` / calib-tune-test 路径下重复计算 overlap。
- 为 `RegularizeScores` 引入 rank 预计算路径，对固定 subset 的 `nonconf` 只做一次排序，后续各 `reg_lambda` 只基于预计算 rank 进行公式计算，不再重复排序。
- 收敛 `BuildStopProfiles` 的内存布局和分配方式，减少 profile 构建中的 `new/free` 和临时容器常数项。
- 保持当前 profile-based Brent 流程、`lamhat` 输出语义、`CalibrationResults` 接口和线上 `CrcStopper` 行为不变。
- 增加基于 perf 的新验收口径，要求新的 preparation 热点从“重复 overlap / rank 排序”转移出来，并量化剩余 Brent 成本。

## Capabilities

### New Capabilities

### Modified Capabilities
- `crc-calibration`: CRC calibration 必须支持基于 subset-level 静态 cache 和 rank 预计算的 runtime preparation 路径，在保持 Brent solver 语义不变的前提下避免重复 overlap 计算和重复 `nonconf` 排序。

## Impact

- 主要影响 `src/index/crc_calibrator.cpp` 中的 calibration 内部数据结构、`RegularizeScores` 的组织方式以及 stop-profile 构建路径。
- 影响 runtime CRC preparation 的耗时和 perf 热点分布，但不改变 `crc_scores.bin`、`CalibrationResults`、`CrcStopper` 或在线 query 主路径接口。
- 验证仍依赖 `bench_e2e` 的 CRC prepare 路径与 query-only perf 口径，重点比较 runtime calibration 总耗时、热点迁移情况以及 `lamhat / actual_fnr / avg_probed / recall` 的稳定性。
