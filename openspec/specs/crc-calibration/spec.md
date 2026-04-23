# CRC Calibration

## Description

ConANN CRC 离线标定流程：给定校准查询集、IVF 索引和 ground truth，输出 `CalibrationResults`，供在线查询早停使用。

## Requirements

### 输入

- 校准查询集 Q_cal: `[n_queries × dim]` float 数组
- IVF 索引: nlist 个聚类的质心 + 每个聚类的向量数据
- Ground truth: 每条查询的真实 top-k 邻居 ID
- 参数: `k` (top-k), `α` (目标 FNR 上界), `dim` (维度)

### 输出

```cpp
struct CalibrationResults {
    float lamhat;        // λ̂
    uint32_t kreg;       // k_reg
    float reg_lambda;    // λ_reg
    float d_min;         // 全局最小 d_p
    float d_max;         // 全局最大 d_p
};
```

### 流程

1. **划分**: Q_cal → calib (50%) / tune (10%) / test (40%)
2. **compute_scores**: 对 calib + tune 集中每条查询:
   - 将 nlist 个聚类按到质心距离排序
   - 逐步 probe 第 1..nlist 个聚类，维护 top-k 堆
   - 记录每步的 `d_p(q)` (当前第 k 近距离) 和 top-k 预测集
3. **全局归一化**: `d_min = min(所有 d_p)`, `d_max = max(所有 d_p)`
4. **nonconformity score**: `nonconf(q, p) = clamp((d_p - d_min) / (d_max - d_min), 0, 1)`
5. **选择 (k_reg, λ_reg)**: 在 tune 集上遍历候选组合 `λ_reg ∈ {0, 0.001, 0.01, 0.1}`，`k_reg ∈ {1, 2, ..., nlist/10}`，对每组做 Brent 求 λ̂，选使平均 probe 数最少的
6. **Brent 求 λ̂**: 在 calib 集上，用选定的 (k_reg, λ_reg)，在 [0, 1] 上求解 `FNR(λ) = α`
7. **评估 (可选)**: 在 test 集上报告实际 FNR 和平均 probe 数

### RAPS 正则化

```
E(q, p) = (1 - nonconf(q, p)) + λ_reg · max(0, p - k_reg)
max_reg_val = 1 + λ_reg · max(0, nlist - k_reg)
reg_score(q, p) = E(q, p) / max_reg_val
```

### Brent 求根

- 区间: [0, 1]
- 目标: f(λ) = FNR(λ) - α = 0
- FNR(λ): 对每条 calib 查询，从 p=1 开始 probe，当 `reg_score(q, p) > λ` 时停在 p，检查此时 top-k 是否包含 GT
- 使用 GSL `gsl_root_fsolver_brent`
- tolerance: 1e-6, 最大迭代: 100

### 约束

- compute_scores 阶段使用精确 L2 距离（不用 RaBitQ 近似）
- 标定结果需要持久化（写入 segment metadata 或单独文件）
- d_min, d_max 从 calib 集全局统计

## ADDED Requirements (from resident-thinpath-batch-rerank-optimization)

### Requirement: CRC calibration 必须作为离线或 prepare 阶段能力存在
CRC calibration MUST 被视为离线构建或 benchmark prepare 阶段能力，不得被在线 E2E benchmark 查询口径隐式包含。在线查询阶段 SHALL 只消费 calibration 结果，而不是重新生成 calibration 结果。

#### Scenario: 在线阶段仅消费 calibration 结果
- **WHEN** 在线 E2E benchmark 开始执行 query round
- **THEN** 系统必须已经具备可直接使用的 CRC calibration 结果

### Requirement: CRC calibration 工件必须可被 benchmark 显式发现和加载
系统 MUST 为在线 benchmark 提供可定位、可校验的 CRC calibration 工件或等价参数存储方式，使 benchmark 能显式判断工件是否存在并据此决定是否可以进入在线查询阶段。

#### Scenario: 工件可见性明确
- **WHEN** benchmark 检查索引目录或相关元数据
- **THEN** 系统必须能够明确判断 CRC calibration 工件是否齐备

## ADDED Requirements (from crc-runtime-calibration-cache)

### Requirement: CRC calibration SHALL reuse subset-level step statistics across reg-lambda evaluation
CRC calibration MUST support a subset-level static cache for query-step statistics that do not depend on `reg_lambda`, including GT-size-equivalent information and step-level overlap or equivalent hit-count statistics. The cache SHALL be reusable across calib, tune, and test evaluation for a fixed subset without recomputing overlap from prediction IDs on every profile build.

#### Scenario: Multiple reg-lambda candidates reuse the same subset statistics
- **WHEN** calibration evaluates multiple `reg_lambda` candidates for the same calib or tune subset
- **THEN** the system MUST reuse precomputed subset-level step statistics instead of recomputing step-level overlap from prediction IDs for each candidate

#### Scenario: Profile build projects overlap from static cache
- **WHEN** a reg-lambda-specific stop profile is built
- **THEN** the overlap contribution used by that profile MUST be derived from the subset-level static cache via the selected step mapping
- **AND** it MUST remain equivalent to the overlap that would have been computed directly from the selected step's predictions and GT

### Requirement: CRC calibration SHALL precompute rank for fixed nonconformity inputs
For a fixed subset and normalization result, CRC calibration MUST support a precomputed rank view of `nonconf(q, p)` so that `RegularizeScores` or its equivalent reg-score path does not need to re-sort the same `nonconf` inputs for every `reg_lambda` candidate.

#### Scenario: Repeated reg-lambda evaluation does not re-sort nonconf
- **WHEN** calibration evaluates multiple `reg_lambda` values on the same normalized query subset
- **THEN** the system MUST reuse a precomputed rank-by-step view of `nonconf`
- **AND** it MUST NOT require a fresh per-query `nonconf` sort for each `reg_lambda`

#### Scenario: Rank-precomputed regularization preserves legacy behavior
- **WHEN** the optimized regularization path is run on the same normalized inputs as the legacy implementation
- **THEN** the resulting reg-score ordering and downstream stop-step behavior MUST remain equivalent or differ only within implementation-level numeric tolerance

### Requirement: Second-step CRC calibration optimization SHALL preserve the profile-based Brent contract
The second-step CRC calibration optimization MUST continue to use the existing profile-based Brent solving contract introduced by the presort optimization. It SHALL reduce repeated cache construction and repeated sorting work inside preparation, but SHALL NOT replace Brent root solving or change the semantics of `CalibrationResults`.

#### Scenario: Cache optimization is enabled
- **WHEN** the cache-optimized calibration path is used
- **THEN** the system MUST continue to solve `lamhat` through the existing Brent-based workflow
- **AND** it MUST continue to emit the same `CalibrationResults` fields as before

#### Scenario: Discrete threshold search remains out of scope for cache optimization
- **WHEN** this cache-oriented optimization step is implemented
- **THEN** the system MUST NOT silently change the calibration solver from Brent to discrete candidate threshold search as part of the same change

## ADDED Requirements (from crc-discrete-threshold-solver)

### Requirement: CRC calibration SHALL support selectable solver modes
CRC calibration MUST support explicit solver selection between `brent` and `discrete_threshold`. The default solver SHALL remain `brent` until a later change explicitly switches the default. Both solver modes MUST reuse the same profile-based `lambda` evaluation path rather than maintaining separate FNR evaluation semantics.

#### Scenario: Benchmark selects the default solver
- **WHEN** CRC calibration is invoked without an explicit solver override
- **THEN** the system MUST use `brent` as the calibration solver

#### Scenario: Benchmark selects the discrete solver
- **WHEN** CRC calibration is invoked with `discrete_threshold`
- **THEN** the system MUST solve `lamhat` using discrete candidate threshold search
- **AND** it MUST continue to use the same `StopProfiles` and `EvaluateLambda` semantics used by the Brent path

### Requirement: Discrete threshold search SHALL operate on actual profile breakpoints
The `discrete_threshold` solver MUST derive its candidate `lambda` set from the actual `reg_score` breakpoint values stored in the active `StopProfiles`. It MUST sort and deduplicate those real breakpoint values before search, and MUST NOT silently replace them with a fixed-step grid or other coarse sampling strategy.

#### Scenario: Candidate thresholds are collected for discrete solving
- **WHEN** the discrete solver prepares its candidate `lambda` set
- **THEN** it MUST collect the candidate thresholds from the real `sorted_scores` values present in the active stop profiles
- **AND** it MUST sort and deduplicate them before evaluating candidates

#### Scenario: No legal candidate satisfies the FNR target
- **WHEN** the discrete solver finds no candidate with `FNR <= target_fnr`
- **THEN** it MUST return the most conservative boundary candidate
- **AND** it MUST NOT silently fall back to Brent solving

### Requirement: Discrete threshold search SHALL minimize avg_probed among legal candidates
For the `discrete_threshold` solver, the selected `lamhat` MUST be the legal candidate with the minimum `avg_probed` among all candidates satisfying `FNR <= target_fnr`. If multiple legal candidates tie on `avg_probed`, the solver SHALL choose the larger `lambda`.

#### Scenario: Multiple legal candidates exist
- **WHEN** more than one candidate satisfies the FNR constraint
- **THEN** the system MUST select the candidate with the smallest `avg_probed`
- **AND** if multiple legal candidates share the same `avg_probed`, it MUST select the larger `lambda`

### Requirement: Brent SHALL remain the default solver until comparison criteria are met
Introducing `discrete_threshold` MUST NOT by itself change the default CRC calibration solver. The default SHALL remain `brent` until a later change explicitly decides to switch the default based on comparison results.

#### Scenario: New solver support is introduced
- **WHEN** the system adds support for `discrete_threshold`
- **THEN** `brent` MUST remain the default calibration solver unless a later change explicitly updates that default

### Requirement: CRC solver provenance SHALL be observable
CRC calibration and benchmark outputs MUST make the active solver and its internal cost visible. The exported output MUST include solver identity and enough statistics to distinguish profile-build cost from solver-loop cost.

#### Scenario: CRC benchmark exports solver provenance
- **WHEN** a benchmark run performs runtime CRC calibration
- **THEN** the result output MUST include `crc_solver`
- **AND** it MUST include `crc_solver_ms`
- **AND** it MUST include `crc_profile_build_ms`
- **AND** it MUST include solver-specific search statistics such as `crc_candidate_count` or `crc_objective_evals`

## ADDED Requirements (from crc-runtime-calibration-presort)

### Requirement: CRC calibration SHALL precompute query stop profiles for repeated lambda evaluation
CRC calibration MUST support a profile-based evaluation path in which each calibration query is transformed once into a reusable stop profile before repeated `lambda` evaluation begins. The stop profile SHALL preserve the existing stop semantics by representing the same `(reg_score, step)` ordering that the current implementation uses during `ComputePredictions`.

#### Scenario: One query is evaluated across multiple lambda candidates
- **WHEN** `PickLambdaReg` or Brent evaluates multiple candidate `lambda` values for the same query set
- **THEN** the system MUST reuse a precomputed per-query stop profile instead of rebuilding and resorting the query's `(reg_score, step)` pairs for each evaluation

#### Scenario: Binary search selects the same stop step as the legacy sorted scan
- **WHEN** a `lambda` value is evaluated against a precomputed stop profile
- **THEN** the selected stop step MUST match the step that would have been chosen by the legacy "sort then take the last `reg_score <= lambda`" rule

### Requirement: CRC calibration SHALL support table-driven FNR accumulation during Brent iterations
During Brent-based `lambda` solving, CRC calibration MUST allow `FNR(lambda)` to be computed through lightweight lookup and accumulation over precomputed query statistics, rather than rebuilding prediction containers on every iteration. Any precomputed hit-count or overlap table used for this path MUST remain equivalent to the overlap that the legacy implementation would have computed from the selected step's predictions and the query's GT set.

#### Scenario: Brent evaluates lambda without rebuilding prediction sets
- **WHEN** Brent invokes the calibration objective for a candidate `lambda`
- **THEN** the system MUST be able to compute the query contribution to `FNR(lambda)` using the selected stop step plus precomputed overlap or equivalent hit-count statistics
- **AND** it MUST NOT require per-iteration reconstruction of full prediction-set containers for every query

#### Scenario: Profile-based FNR matches legacy calibration behavior
- **WHEN** calibration runs on the same `crc_scores` input, split, and solver configuration before and after the profile-based optimization
- **THEN** the resulting `lamhat`, `actual_fnr`, and `avg_probed` MUST remain equivalent or differ only within implementation-level numeric tolerance

### Requirement: First-step CRC calibration optimization SHALL preserve the Brent solver contract
The first-step CRC calibration optimization MUST preserve the current Brent-based solving contract and SHALL NOT replace it with discrete candidate threshold search. The optimization MUST therefore be limited to reducing repeated work inside `FNR(lambda)` evaluation while keeping `PickLambdaReg`, Brent root finding, and `CalibrationResults` semantics unchanged.

#### Scenario: First-step optimization is enabled
- **WHEN** the optimized calibration path is used
- **THEN** the system MUST continue to solve `lamhat` with the existing Brent-based workflow
- **AND** it MUST continue to emit the same `CalibrationResults` fields as the legacy path

#### Scenario: Discrete threshold search remains out of scope
- **WHEN** this change is implemented
- **THEN** the system MUST NOT silently switch calibration from Brent solving to discrete candidate threshold search as part of the same optimization step
