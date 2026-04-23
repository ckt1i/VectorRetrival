## 1. Solver Plumbing

- [x] 1.1 在 `CrcCalibrator::Config` 中增加 solver 枚举字段，并保持默认值为 `brent`
- [x] 1.2 在 `bench_e2e` 增加 CRC solver CLI 参数，并接入 runtime calibration 配置
- [x] 1.3 保持 `CalibrationResults` 与 `CrcStopper` 接口不变，仅在 calibration 内部切换 solver

## 2. Discrete Solver Implementation

- [x] 2.1 基于现有 `StopProfiles` 实现真实 breakpoint 候选集合收集与去重
- [x] 2.2 实现 `SolveDiscreteThreshold`，固定采用“合法解中 `avg_probed` 最小、平手取更大 `lambda`”的规则
- [x] 2.3 将 `PickLambdaReg` 和 final solve 改为通过 solver dispatch 选择 `brent` 或 `discrete_threshold`
- [x] 2.4 保持 tune/test eval 统一复用 `EvaluateLambda`，不引入第二套评估语义

## 3. Metrics And Logging

- [x] 3.1 为 calibration 记录 solver provenance 和成本字段：`crc_solver`、`crc_candidate_count`、`crc_solver_ms`、`crc_profile_build_ms`、`crc_objective_evals`
- [x] 3.2 将上述字段写入 `bench_e2e` 日志、结果 JSON 和 CRC calibration artifact

## 4. Comparison Validation

- [x] 4.1 编译并验证 `bench_e2e` 在 `brent` 与 `discrete_threshold` 下都能完成 runtime CRC calibration
- [x] 4.2 跑 `alpha ∈ {0.01, 0.02, 0.05}`、`seed ∈ {42, 43, 44}` 的 Brent vs discrete 对照
- [x] 4.3 汇总 `lamhat`、`actual_fnr`、`avg_probed`、`online recall@10`、`avg_probed_clusters` 和 solver 成本，判断是否值得后续切换默认 solver

<!--
Comparison summary:
- Brent and discrete_threshold produced nearly identical lamhat / actual_fnr / avg_probed / online recall / avg_probed_clusters.
- No meaningful online gain was observed from discrete_threshold.
- discrete_threshold calibration cost was much higher:
  * seed 42: candidate_count ~1.5e4, solver_ms ~37-50 ms
  * seed 43/44: candidate_count ~2.5e6, solver_ms ~168-183 ms
  * Brent solver_ms stayed around ~0.7-2.6 ms
- Conclusion: keep the discrete solver implementation for comparison, but retain Brent as the default solver.
-->
