# E2E Benchmark Spec

## Overview

End-to-end benchmark infrastructure for vector search, supporting both sliding-window and full-preload cluster loading modes.

## Requirements

### Requirement: E2E benchmark output SHALL support `.clu` loading-mode comparison
The E2E benchmark workflow SHALL support direct before/after comparison between the existing sliding-window `.clu` loading path and the new full-preload `.clu` loading path under the same warm-serving query settings.

#### Scenario: Same operating point is run under both loading modes
- **WHEN** the benchmark evaluates the `.clu` preload optimization
- **THEN** it SHALL run the same dataset and search parameters under both `window` and `full_preload` modes
- **AND** the exported results SHALL make the loading mode explicit

#### Scenario: Query-speed comparison fields are exported
- **WHEN** a loading-mode comparison result is exported
- **THEN** the result SHALL include `recall@10`, `e2e_ms`, and `p99_ms`
- **AND** it SHALL include enough metadata to reconstruct the exact operating point being compared

#### Scenario: Preload cost is recorded alongside query results
- **WHEN** `full_preload` mode is benchmarked
- **THEN** the benchmark output SHALL record the preload time and preload-related resident memory or byte footprint
- **AND** these fields SHALL be reported alongside the query-speed result rather than omitted

## CLI Parameters

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--dataset` | string | `/home/zcq/VDB/data/coco_1k` | 数据集路径 |
| `--queries` | int | 0 | query 数量限制 |
| `--output` | string | `/home/zcq/VDB/test/` | 输出基础目录 |
| `--nlist` | int | 32 | K-Means 聚类数 |
| `--nprobe` | int | 32 | 搜索探查聚类数 |
| `--topk` | int | 10 | Top-K |
| `--early-stop` | int | 1 | 启用 early stop (1=on, 0=off) |
| `--bits` | int | 1 | RaBitQ 量化位数 |
| `--block-size` | int | 64 | RaBitQ 块大小 |
| `--c-factor` | float | 5.75 | RaBitQ c 因子 |
| `--max-iter` | int | 20 | K-Means 最大迭代 |
| `--seed` | int | 42 | 随机种子 |
| `--page-size` | int | 4096 | 存储页大小 |
| `--p-for-dk` | int | 99 | d_k 校准百分位 |

## Behavior Constraints

- 所有参数的默认值必须与当前硬编码值一致，确保无参数调用时行为不变
- 新增 `GetFloatArg` 辅助函数支持 float 类型参数
- config.json 输出中包含所有参数值（含默认值）

## Additional Requirements (from fastscan-epsilon-bound-validation)

### Requirement: E2E benchmark 必须报告 epsilon 验证元数据
warm-serving benchmark 的输出必须包含足够的元数据，用来区分构建阶段的 epsilon 输入和运行时加载到的 FastScan 边界。

#### Scenario: 输出中可见加载后的 epsilon
- **当** `bench_e2e` 在重建索引或预构建索引上运行时
- **则** 输出中必须包含加载后的运行时 `eps_ip`
- **并且** 输出中必须标明这次运行使用的是重建后的索引，还是通过 `--index-dir` 复用的索引

### Requirement: E2E benchmark 必须报告 Stage 1 和 Stage 2 的候选流统计
用于 epsilon 验证的 benchmark 输出必须包含分析候选流变化所需的 Stage 1 和 Stage 2 计数，而不能只输出最终延迟。

#### Scenario: 候选流统计被导出
- **当** 一次 epsilon 验证运行结束时
- **则** 输出中必须包含 `avg_total_probed`
- **并且** 输出中必须包含 Stage 1 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `Uncertain`
- **并且** 输出中必须包含 `avg_probe_time_ms`
