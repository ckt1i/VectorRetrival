## Why

现有 presort/profile 版本已经把 CRC calibration 的 `ComputePredictions + sort` 退出主热点，但 solver 仍然沿用 Brent 在连续区间上求根。由于 `FNR(lambda)` 本质上只会在真实 `reg_score` 跳变点上发生变化，下一步需要单独评估：在共享同一套 `StopProfiles` 和 `EvaluateLambda` 的前提下，离散候选阈值搜索是否比 Brent 更稳定、更贴合目标，并是否值得后续切换默认 solver。

此前的 presort/cache change 都明确把“保持 Brent 语义不变”作为边界，因此这一步必须新开一个独立 change 来记录 solver 语义变化，并把实现、日志与对照实验放在同一个明确范围内。

## What Changes

- 在 CRC calibration 中新增可切换的 solver 模式：`brent` 和 `discrete_threshold`，默认仍为 `brent`。
- 让离散 solver 复用现有 `StopProfiles` 和 `EvaluateLambda`，候选集合直接来自所有真实 `reg_score` 跳变点，而不是固定步长网格。
- 定义离散 solver 的选点规则：在满足 `FNR <= target_fnr` 的候选中，优先选择 `avg_probed` 最小者；若平手，则选更大的 `lambda`；若无合法候选，则选择最保守边界候选，不回退 Brent。
- 在 `PickLambdaReg` 和 final solve 两处统一接入 solver dispatch，使 tune/test eval 始终复用同一套 profile 评估逻辑。
- 扩展 benchmark 与结果输出，记录 solver provenance 和内部成本字段，包括 `crc_solver`、`crc_candidate_count`、`crc_solver_ms`、`crc_profile_build_ms`、`crc_objective_evals`。
- 本 change 不切换默认 solver，不改变 `CalibrationResults`、`CrcStopper`、`crc_scores.bin` 或 `StopProfiles` 主结构。

## Capabilities

### New Capabilities

### Modified Capabilities
- `crc-calibration`: CRC calibration 必须支持可切换的 solver 路径，在保持 profile 评估口径一致的前提下支持 Brent 和离散候选阈值搜索对照。
- `e2e-benchmark`: Benchmark 输出必须能够记录并区分 runtime CRC calibration 所使用的 solver 与其内部成本统计。

## Impact

- 主要影响 `include/vdb/index/crc_calibrator.h`、`src/index/crc_calibrator.cpp` 和 `benchmarks/bench_e2e.cpp` 中的 CRC calibration solver 选择、日志与结果输出。
- 不影响 `CalibrationResults` 和 `CrcStopper` 的外部消费方式，也不改变索引构建产物或 `crc_scores.bin` 格式。
- 验证将基于现有 `bench_e2e` CRC 路径，在固定 `nprobe / alpha / seed` 条件下对比 Brent 与 discrete solver 的 `lamhat`、`actual_fnr`、`avg_probed`、`online recall@10` 和 `avg_probed_clusters`。
