## Why

第 0 阶段 baseline 对比和 submit-path 优化已经把 BoundFetch 的 warm E2E 从约 `5.42ms` 降到约 `1.136ms`，同时也暴露出当前真正的问题不再是 cold-start 或设备 I/O wait，而是如何在 `warm steady-state` 下把 recall-latency tradeoff 讲清楚并继续提升 recall。现有实验计划仍夹杂 cold-start、超大规模 benchmark 扩张和过多附属方向，已经不适合当前阶段。

## What Changes

- 将后续实验路线正式收束为 `warm steady-state only`，不再把 cold-start / drop-cache 作为必测项。
- 新增一套 `warm-serving` 评测能力，明确主结果、机制归因和 recall 提升消融的最小实验闭环。
- 修改现有 `e2e-benchmark` 要求，使其支持：
  - 以 COCO 100K 为主的 warm Pareto 评测
  - 更密集的 BoundFetch operating point 采样
  - 固定 reporting 字段，用于 recall-latency、submit/probe breakdown 和 appendix 验证
- 将后续任务重新排序：
  - 先做主结果 Pareto
  - 再做机制归因
  - 最后用最小 recall-improvement ablation 判断是否还值得继续优化代码

## Capabilities

### New Capabilities
- `warm-serving-evaluation`: 定义 BoundFetch 在 warm steady-state 下的主评测协议、主结果、消融顺序和停机门槛

### Modified Capabilities
- `e2e-benchmark`: 将端到端 benchmark 的主协议和输出要求调整为 warm-only、Pareto-first、机制归因为辅

## Impact

- Affected docs:
  - `refine-logs/FINAL_PROPOSAL.md`
  - `refine-logs/EXPERIMENT_PLAN.md`
  - `refine-logs/EXPERIMENT_TRACKER.md`
  - 中文对应文档
- Affected benchmark workflow:
  - `benchmarks/bench_e2e.cpp`
  - `baselines/results/e2e_comparison_warm.csv`
  - `baselines/results/analysis.md`
- Affected execution flow:
  - 后续实现会以 COCO 100K warm E2E、机制 breakdown、recall tuning 为主
  - qd / SQPOLL / isolated/shared 进入 appendix 或冻结验证，不再是主线
