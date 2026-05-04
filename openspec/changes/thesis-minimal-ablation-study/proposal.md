## Why

`thesis-minimal-main-sweep` 已完成 `T000-T015` 的主实验主矩阵（`coco_100k`、`msmarco_passage`），但主实验之外仍需要一套可追踪的后续机制消融与 top-k 稳健性实验合同，以避免消融实验把主实验 change 打开或污染主结果口径。

## What Changes

- 新建一条独立的论文消融研究 change，执行 `refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_PLAN.md` 中主实验后的区块：
  - 结果卫生（result hygiene）与代表点冻结
  - BoundFetch 三类 triage 删除式消融
  - 动态调度消融
  - top-k 敏感性补充
- 主论文表继续固定为 `topk=10`；`topk=20/50/100` 作为鲁棒性/补充证据，不作为主 sweep 的替代。
- 消融实验统一基于主线冻结的主结果点，而不是重新展开完整 `dataset × system × nprobe × topk` 矩阵。
- 每个消融结果必须记录足够可追溯信息，至少包含：
  - 数据集
  - canonical artifact
  - nlist/nprobe
  - topk
  - BoundFetch 变体或 scheduler 策略
  - recall 与 latency 指标
  - 可用时包含 triage、rerank、payload 与 I/O 计数
- 将消融范围从 `thesis-minimal-main-sweep` 中剥离，使主实验 change 仅保留 `T000-T015`、`topk=10`、canonical 索引复用与主表跟踪。

## Capabilities

### New Capabilities
- `thesis-minimal-ablation-study`：定义论文主实验后续的结果卫生、triage 消融、调度消融、top-k 敏感性、指标、可追溯性与汇报合同。

### Modified Capabilities
- 无。`thesis-minimal-main-sweep` 的范围清理直接体现在该 change 的 proposal/design/spec 文件中，不通过本 change 再做 capability 修改。

## Impact

- 受影响的 OpenSpec change：
  - `openspec/changes/thesis-minimal-main-sweep`
  - `openspec/changes/thesis-minimal-ablation-study`
- 受影响的规划文档：
  - `refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_PLAN.md`
  - `refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_TRACKER.md`
- 受影响的实验产物：
  - `/home/zcq/VDB/baselines/formal-study/outputs/vector_search/*`
  - `/home/zcq/VDB/baselines/formal-study/outputs/e2e/*`
  - `/home/zcq/VDB/baselines/formal-study/trackers/*`
- 受影响代码/脚本（仅缺少标志位或聚合字段时）：
  - `benchmarks/bench_e2e.cpp`
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/run_vector_search/*`
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/run_e2e_coupled/*`
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/aggregate/*`
