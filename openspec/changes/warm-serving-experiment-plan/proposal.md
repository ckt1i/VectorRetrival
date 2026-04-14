## Why

第 0 阶段 baseline 对比和 submit-path 优化已经把 BoundFetch 的 warm E2E 从约 `5.42ms` 降到约 `1.136ms`，同时也暴露出当前真正的问题不再是 cold-start 或设备 I/O wait，而是如何在 `warm steady-state` 下把 recall-latency tradeoff 讲清楚并继续提升 recall。现有实验计划仍夹杂 cold-start、超大规模 benchmark 扩张和过多附属方向，已经不适合当前阶段。

## What Changes

- 将后续实验路线正式收束为 `warm steady-state only`，不再把 cold-start / drop-cache 作为必测项。
- 新增一套 `warm-serving` 评测能力，明确主结果、机制归因和 recall 提升消融的最小实验闭环。
- 修改现有 `e2e-benchmark` 要求，使其支持：
  - 以 COCO 100K 为主的 warm Pareto 评测
  - BoundFetch 的多参数 operating point 采样
  - 两条 baseline 的参数化 sweep 与 Pareto 对比
  - 固定 reporting 字段，用于 recall-latency、submit/probe breakdown 和 appendix 验证
- 将后续任务重新排序：
  - 先做主结果 Pareto
  - 再做机制归因
  - 最后用最小 recall-improvement ablation 判断是否还值得继续优化代码
- 将 proposal 细化为可执行实验方案，明确：
  - 每个实验块的固定数据集、查询数、参数 sweep 范围
  - 每个实验块的输出文件和结果记录位置
  - 每个实验块的通过标准与停止条件
  - 每轮实验完成后需要更新的分析文档与 tracker

## Capabilities

### New Capabilities
- `warm-serving-evaluation`: 定义 BoundFetch 在 warm steady-state 下的主评测协议、主结果、消融顺序和停机门槛
- `baseline-pareto-comparison`: 定义两条 baseline 曲线的参数 sweep、对齐方式和输出要求

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
  - BoundFetch 主曲线不再只扫 `nprobe`，而是以 `nprobe={200,500}` 为骨架，联合考虑 `crc-alpha={0.01,0.02,0.05,0.08,0.1,0.15,0.2}`、`epsilon-percentile={0.99,0.95,0.9}` 和 `nlist={1024,2048}`
  - 两条 baseline 也需要各自扫参数，构造可比较的 Pareto 曲线，并形成可直接分派的执行表
  - qd / SQPOLL / isolated/shared 进入 appendix 或冻结验证，不再是主线
- Affected artifacts:
  - `refine-logs/EXPERIMENT_PLAN.md` / `EXPERIMENT_TRACKER.md`
  - `refine-logs/EXPERIMENT_PLAN_CN.md` / `EXPERIMENT_TRACKER_CN.md`
  - `baselines/results/e2e_comparison_warm.csv`
  - `baselines/results/analysis.md`
