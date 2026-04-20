## Context

当前系统已经完成多轮 query-path 优化，并且 benchmark 已经能稳定输出 `coarse_select_ms`、`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms`、`probe_submit_ms`、prefetch 与 rerank 相关细分字段。基于最近一轮 full E2E 和 query-only perf，`prepare/stage1` 之外仍值得继续投入的部分主要集中在：

- `probe_submit` 链路，包括 candidate 去重、slot 分配、`PrepRead*` 组织与提交。
- `coarse_select` 的后续收敛，包括 score 后段搬运和 top-n 前后的 cache / scratch 组织。
- `probe_stage2 / IPExRaBitQ` 的常数项压缩和 query 无关状态前移。
- prefetch / `io_uring` 提交与等待链路。
- 在前述热点进一步下降后，再重新评估 rerank / payload 后段是否升为主瓶颈。

因此这次 change 的目标不是立刻实现全部候选点，而是先把这些后续优化点整理成一个可持续展开的 backlog change，并把 `probe_submit` 的前三个最高优先级工作包直接固化进文档，作为后续实现的首轮入口：

1. batched submit skeleton
2. batch-aware dedup
3. CRC / 微计时移出 submit hotpath

## Goals / Non-Goals

**Goals:**

- 形成一份按 ROI 排序的 query 后续优化列表。
- 为每个优化点给出预估收益区间和依赖关系。
- 固定后续逐项优化时必须使用的 benchmark / perf 口径。
- 把 `probe_submit` 的前三个优先项细化到可直接进入实现设计的粒度。
- 让后续任一优化子项都可以从这份 backlog 中独立拆出或直接细化执行。

**Non-Goals:**

- 本 change 不直接承诺实现任何具体内核或调度改造。
- 本 change 不修改当前 recall、排序、payload 语义或 CLI 行为。
- 本 change 不重新定义已有统计字段的边界，只约束后续优化要沿用现有口径。

## Decisions

### 决策一：将本轮工作定义为 backlog 型 change，而不是单点实现 change

把多个剩余热点收敛到一个 backlog change 里，可以避免每次新讨论都重新解释背景、口径和优先级。后续无论是继续开子 change，还是直接在 backlog 上增补 design / tasks，都有统一落点。

替代方案是继续为每个点单独起 change；问题是当前很多点仍处于“待进一步分析”阶段，单独起 change 会过早承诺实现边界。

### 决策二：优先级以 query-only perf 的 CPU 热点和 full E2E 的平均时延共同决定

单看 full E2E wall time，容易受到 GT、I/O 抖动和结果导出路径影响；单看 query-only perf，又可能忽略真正影响端到端体验的等待段。因此 backlog 中每个优化点都同时记录：

- query-only 视角下的热点归因
- full E2E 视角下对 `avg_query_ms` 的预估改善

### 决策三：保持“先主路径、后尾段”的顺序

在当前结果下，`probe_submit`、`coarse_select` 后续收敛和 `probe_stage2` 仍然比 rerank / payload fetch 更值得优先处理。因此 backlog 默认推荐顺序为：

1. `probe_submit`
2. `coarse_select`
3. `probe_stage2`
4. prefetch / `io_uring`
5. rerank / payload 后段

### 决策四：`probe_submit` 首轮按“三步走”推进，而不是一次性混做

`probe_submit` 当前最值得优先处理的三项工作包分别是：

1. `OnCandidates(batch)` 改成真正的 batch submit
2. dedup 改成 batch-aware 结构
3. CRC heap 更新与 per-candidate 微计时从 submit 热路径拆出

三项的依赖顺序固定为：

- 第一步先做 batched submit skeleton，保留当前 dedup 结构和 CRC 语义
- 第二步在 batched submit 结构上引入 batch-local dedup 和更轻的 query 级 dedup 结构
- 第三步再把 CRC heap 更新和微计时从 per-candidate 改成 batch / cluster 粒度

这样做的原因是，第一步提供最大的结构性收益，后两步则是在新骨架上继续压缩常数项。

### 决策五：将收益记录为区间估计，而不是单值承诺

这些优化点的真实收益受 query 数、`nprobe`、resident 模式、候选量以及 I/O 重叠情况影响较大，因此 backlog 只记录收益区间，例如 `-0.015 ~ -0.045 ms`，不把单次机器上的某个数值写成确定承诺。

对于 `probe_submit` 的三项首轮工作，当前估计区间记录为：

- batched submit skeleton：`avg_query_ms` 约 `-0.015 ~ -0.030`
- batch-aware dedup：`avg_query_ms` 约 `-0.006 ~ -0.015`
- CRC / 微计时移出 hotpath：`avg_query_ms` 约 `-0.005 ~ -0.012`

### 决策六：首轮 `probe_submit` 改造不改变结果语义

这一轮对 `probe_submit` 的细化只允许改变提交组织方式，不允许改变：

- SafeIn / Uncertain 分类语义
- rerank 语义
- payload 路径语义
- recall / 排序语义

也就是说，允许重排提交结构，但不允许把这三项优化和其它语义改造混在一起。

## Risks / Trade-offs

- [Backlog 过于宽泛] → 在 tasks 中按优先级拆成独立分析与决策步骤，不把所有点一次性展开。
- [预估收益失真] → 明确所有收益仅作为当前口径下的区间估算，后续必须由同口径 full E2E + query-only perf 复核。
- [后续实现边界模糊] → 在 backlog 中记录每个点的前置条件和不建议提前做的原因，例如 rerank 当前不作为优先项。
- [batched submit 改动面偏大] → 第一阶段先保留旧 dedup 和 CRC 逻辑，只替换提交骨架，降低一次性改动风险。
- [dedup 改造可能影响统计解释] → 在后续实现中区分 batch-local duplicate 与 global dedup，避免统计口径混淆。
- [CRC 下沉可能误伤 early stop 语义] → 只允许下沉到 cluster-end merge，不允许推迟到 all-probe-done 之后。

## Migration Plan

1. 先落这份 backlog change，作为后续 query 优化的统一入口。
2. 先以 `probe_submit` 的三项优先工作包作为首轮实施对象，按 `batch submit -> dedup -> CRC/timing off hotpath` 的顺序推进。
3. 后续每次选择一个优化点时，先基于 backlog 进一步细化设计或单独开子 change。
4. 每完成一项优化，都回填同口径 benchmark / perf 结果，并据此调整 backlog 优先级。

## Open Questions

- `probe_submit` 的 batched submit skeleton 是否只在 `AsyncIOSink` 内部重构，还是需要继续向 `AsyncReader` 引入更轻的批量 prep helper。
- `coarse_select` 下一步是否仍止于 scratch / 搬运收敛，还是需要进入 top-n selection 改写。
- `probe_stage2` 的常量项压缩是否足以带来可见收益，还是需要更激进的 kernel 级改写。
