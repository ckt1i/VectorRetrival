## Context

当前 BoundFetch 已完成 baseline 对比和一轮关键优化，最新 warm 结果表明：

- BoundFetch 在 `coco_100k` 上达到约 `1.136ms @ recall@10=0.898`
- DiskANN+FlatStor 的对应 warm 点约为 `3.790ms @ recall@10=1.000`
- `io_wait_ms` 已接近 0，主要成本转移到 `uring_submit_ms` 和 `probe_ms`
- `io_queue_depth` 继续增大没有稳定收益
- `SQPOLL` 在当前环境回退，不能作为主收益来源

同时，服务器不提供 `sudo`，且项目方明确表示 cold-start 不是现实生产环境下的核心目标。因此后续设计必须把实验协议、验收标准和执行顺序全部收束到 warm steady-state。

## Goals / Non-Goals

**Goals:**
- 定义唯一主协议：warm steady-state
- 让后续实验直接服务于两件事：
  - BoundFetch 的 warm E2E Pareto 是否成立
  - 是否还能在接近当前延迟下显著提升 recall
- 固定最小执行顺序和停机门槛，避免继续扩张为大而全 benchmark 套件
- 将已有 qd / mode / SQPOLL 结果降级为附录或冻结项

**Non-Goals:**
- 不再要求 cold-start / drop-cache / semi-cold
- 不重新展开超大数据集 sweep 作为主线
- 不把更多低层 `io_uring` 微优化作为主任务
- 不把 SafeIn 激活条件探索作为主结果中心

## Decisions

### 1. 以 warm-only 取代 cold/warm 双协议

选择：
- 所有主结果、主表、主图只使用 warm steady-state

原因：
- 与部署现实一致
- 与当前权限环境一致
- 避免继续为不服务于最终论文故事的协议投入工程时间

备选方案：
- 保留 cold 作为“理想附录协议”
- 否决原因：会持续制造未完成负担，也会稀释主故事

### 2. 主结果采用 Pareto-first，而不是单点对比

选择：
- 用 BoundFetch `nprobe` sweep 与强 baseline 点构成 recall-latency Pareto

原因：
- 当前最主要的争议不在“能不能更快”，而在“更快是否只是用 recall 换来的”
- Pareto 比单点更能支撑论文主张

备选方案：
- 继续只保留最佳单点
- 否决原因：容易被 reviewer 质疑选点偏置

### 3. 将机制归因单独做成最小支撑块

选择：
- 用 `uring_submit_ms / probe_ms / io_wait_ms / submit_calls` 构成一张主归因图或表

原因：
- 现在需要解释“为什么快”
- 但不能让整篇工作退化成 profiling note

备选方案：
- 展开更多 perf / kernel tracing
- 否决原因：投入大、收益小、容易转移论文重心

### 4. 把后续代码优化门槛改成 recall-improvement gate

选择：
- 仅当小规模阈值/策略调整能在近似延迟下显著提高 recall 时，继续优化代码

原因：
- 当前 submit-path 已经吃掉大部分显性收益
- 继续做系统微优化的边际回报不明

备选方案：
- 持续做 deeper submit-path / qd / SQPOLL 优化
- 否决原因：已有实验已经显示这些方向不是当前主要收益点

## Risks / Trade-offs

- [BoundFetch 仍然低于强 baseline 的高 recall 区域] → 必须优先完成 recall-improvement ablation，并用 Pareto 而非单点描述结果
- [论文被看成“调参日志”] → 把 qd / mode / SQPOLL 固定为 appendix supporting evidence，不继续扩张
- [只有 COCO 主数据集会被质疑泛化不足] → 在主结果站稳后，用 Deep1M 或 Deep8M 做轻量支撑，不做全套 sweep
- [warm-only 会被质疑规避最难场景] → 在 proposal 和 spec 中明确 cold-start 是 non-goal，且系统目标是 production-style steady-state serving

## Migration Plan

1. 更新变更文档与实验计划，统一为 warm-only
2. 固定主 benchmark 输出字段和主评测参数
3. 先补 Pareto 缺失点，再跑最小 recall 提升消融
4. 若 recall 没有改善空间，则冻结代码，转入写作和图表整理
5. 若 recall 可以改善，再决定是否开新的优化 change

## Open Questions

- 当前最值得补的中间 `nprobe` 点是 `150/250`，还是应该优先扫 `CRC_alpha`？
- recall 提升最可能来自：
  - `CRC_alpha`
  - pruning threshold
  - rerank / verification policy
  需要先选一个最小变量集
- Deep1M 与 Deep8M 中，哪个更适合作为 appendix generality check？
