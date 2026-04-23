## Context

当前 CRC calibration 已经完成 presort/profile 化，`ComputePredictions + sort` 不再是主热点。现有路径通过 `StopProfiles + EvaluateLambda` 评估 `FNR(lambda)`，但求解器仍然是 Brent，在 `[0, 1]` 上把 `FNR(lambda) - target_fnr` 视为连续根问题来处理。由于 `FNR(lambda)` 实际只会在真实 `reg_score` 跳变点上发生变化，这一问题本质上更接近离散候选阈值搜索，而不是连续数值求根。

此前两个 CRC calibration change 都明确把“保持 Brent 语义不变”作为边界，因此如果要评估离散候选阈值搜索，必须新开一个独立 change。这样才能把“profile 组织优化”和“solver 语义替换”分开验证，避免收益归因混乱。

## Goals / Non-Goals

**Goals:**
- 在现有 `StopProfiles` 与 `EvaluateLambda` 基础上新增 `discrete_threshold` solver，与 `brent` 并存可切换。
- 让离散 solver 的候选集合只来自真实 `reg_score` 跳变点，而不是粗网格近似。
- 把 `PickLambdaReg` 和 final solve 统一接到 solver dispatch 上，使 tune/test eval 继续复用同一套 profile 评估逻辑。
- 在 benchmark 与结果输出中记录 solver provenance，支持对 Brent 和 discrete solver 做一一对照。
- 保持默认 solver 为 `brent`，本 change 只提供对照能力，不默认切换主路径。

**Non-Goals:**
- 不改变 `CalibrationResults`、`CrcStopper`、`crc_scores.bin` 或 `StopProfiles` 主结构。
- 不把离散 solver 与后续 profile 布局/内存优化混成一次改动。
- 不在本次 change 中决定是否将默认 solver 从 Brent 切到 discrete。
- 不改变线上 stop 公式，也不处理 offline/online 语义一致性问题。

## Decisions

### Decision: solver 模式使用显式枚举，默认 `brent`
在 `CrcCalibrator::Config` 中新增 solver 枚举字段，取值固定为：
- `brent`
- `discrete_threshold`

默认值为 `brent`。`bench_e2e` 提供对应 CLI 参数，只在显式指定时启用离散 solver。

### Decision: 离散候选集合直接来自真实 breakpoint
离散 solver 的候选集合由 `StopProfiles` 中所有 query 的 `sorted_scores` 汇总得到。实现步骤固定为：
- 收集全部 `sorted_scores`
- 排序
- 去重

不允许使用固定步长网格作为默认候选集合，也不允许在本次 change 中 silently fallback 到粗采样近似。

### Decision: 选点规则固定为 “合法解中 avg_probed 最小”
对于离散 solver，给定 `target_fnr` 和候选集合，选点规则固定为：
- 先筛选 `FNR <= target_fnr` 的合法候选
- 在合法候选中取 `avg_probed` 最小者
- 若有平手，取更大的 `lambda`
- 若不存在合法候选，则取最保守边界候选
- 不回退 Brent

### Decision: solver dispatch 只放在 `PickLambdaReg` 和 final solve 两处
整个 calibration 流程保持不变：
- split calib/tune/test
- normalize
- `PickLambdaReg`
- final solve
- test eval

只有两处求解 `lamhat` 的位置通过 `config.solver` 选择：
- `PickLambdaReg` 里在 calib set 上求候选 `lamhat`
- final solve 在 calib set 上求最终 `lamhat`

tune/test 评估统一使用 `EvaluateLambda`，不为离散 solver 新写第二套评估逻辑。

### Decision: benchmark 必须导出 solver provenance 和成本统计
为了后续判断是否切换默认 solver，benchmark 和运行结果必须导出：
- `crc_solver`
- `crc_candidate_count`
- `crc_solver_ms`
- `crc_profile_build_ms`
- `crc_objective_evals`

其中：
- Brent 下 `crc_candidate_count` 可以为 0
- Discrete 下 `crc_objective_evals` 可以为 0 或省略

## Risks / Trade-offs

- [离散 solver 选到的 `lamhat` 与 Brent 数值差异明显] → 允许 `lamhat` 数值不同，但比较时优先看 `actual_fnr / avg_probed / online recall / avg_probed_clusters` 行为。
- [候选集合过大导致 discrete solver 自身开销偏高] → 先实现真实 breakpoint 搜索并记录 `candidate_count`；若后续发现过大，再单独讨论压缩策略。
- [没有合法候选时行为不清晰] → 规则固定为取最保守边界候选，不回退 Brent。
- [默认值直接切到 discrete 会放大风险] → 默认继续使用 `brent`，本次只增加显式可切换路径和对照日志。

## Outcome

已按设计完成 `brent` 与 `discrete_threshold` 的最小对照矩阵：

- operating point: `nprobe=512`, `queries=1000`, `full_preload + resident=1`
- `alpha ∈ {0.01, 0.02, 0.05}`
- `seed ∈ {42, 43, 44}`

结果结论如下：

- `discrete_threshold` 在行为上基本复刻了 Brent：`lamhat`、`actual_fnr`、`avg_probed`、`online recall@10`、`avg_probed_clusters` 几乎一致。
- `discrete_threshold` 没有带来足够明显的 online 收益，未出现稳定的 `avg_probed_clusters` 或 recall 改善。
- `discrete_threshold` 的 calibration solver 成本显著高于 Brent。
  - seed `42` 上候选集合约 `1.5e4`，`solver_ms` 约 `37~50 ms`
  - seed `43/44` 上候选集合约 `2.5e6`，`solver_ms` 约 `168~183 ms`
  - 对应 Brent 的 `solver_ms` 通常仅 `0.7~2.6 ms`

因此，本 change 的最终判断是：

- 保留 `discrete_threshold` 作为显式可切换、可对照的实验路径
- 默认 solver 继续保持 `brent`
- 不需要回退本次代码实现，但也不应基于当前结果切换默认 solver
- 如果后续还要继续优化，优先方向应回到 candidate-space/profile-build 成本压缩，而不是默认 solver 替换
