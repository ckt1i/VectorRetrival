## Context

最近一轮 `full_preload + use_resident_clusters=1 + nprobe=64 + crc=1` 的 benchmark 与 query-only perf 已经把主路径热点拆得比较清楚。当前平均 query 时间大约由三块主成本构成：`coarse_select_ms` 约 0.70ms，`probe_prepare_ms` 约 0.10ms，`probe_stage1_ms` 约 0.50ms；`probe_submit_ms` 仍然有可见成本，但已经小于前面三项。perf 也表明热点不再集中在 rerank，而更多集中在 `PrepareQueryRotatedInto`、`BuildFastScanLUT`、`EstimateDistanceFastScan`、resident probe 包装路径和候选提交组织。

这意味着下一轮优化不该继续从 rerank 入口切，而应该优先压薄 resident query wrapper、FastScan Stage1 和 coarse select。另一方面，这一轮优化不能破坏当前已经建立起来的细化统计口径；实现完成后必须立即用同参数 full E2E 与 query-only perf 复测，否则无法判断收益落在哪一层。

约束上，本轮仍以 resident 单 assignment 路径为核心验收路径，不改 coarse clustering 规则，不改 recall 语义，不引入新的实验协议。重点是把 query 主路径中的可复用数据结构前移，把在线阶段保留成更接近 RaBitQ-Library 那种“query wrapper + in-memory scan”的薄路径，但不牺牲当前系统对 prefetch、payload 和 resident 运行方式的兼容。

## Goals / Non-Goals

**Goals:**
- 为 resident 常驻聚类模式建立更薄的 query wrapper / hot path，减少每次 query 重复构造的对象和中间态。
- 压缩单 bit FastScan 的准备与 Stage1 分类成本，优先处理 `PrepareQueryRotatedInto`、LUT 构建和 Stage1 主循环。
- 在 resident + single assignment 条件下弱化不必要的全局去重与高频小粒度提交成本，降低 `probe_submit_ms`。
- 对 coarse select 建立单独的优化约束和统计边界，确保它不会继续和 probe 路径混在一起。
- 保持当前 benchmark 输出字段，并把这轮优化后的 full E2E / query-only perf 结果纳入正式验收。

**Non-Goals:**
- 不在本轮改变 CRC 生命周期、batch rerank 语义或 payload 流程，它们已有独立 change 负责。
- 不直接切换到外部 `RaBitQ-Library` 作为生产查询实现；本轮只参考其 query 数据流组织方式。
- 不修改索引格式、不重写 coarse builder，也不改变 single assignment 的聚类归属语义。
- 不承诺本轮直接达到最终目标时延，只要求明确压缩 P0 主路径并留下可解释的 perf 证据。

## Decisions

### 决策一：把 resident query path 进一步收敛为固定容量、可复用的 query wrapper

实现上，为 resident 模式增加专用 query wrapper / scratch 上下文。该上下文在 prepare/init 阶段或线程首次进入时完成容量保留与字段布局确定，在线查询只做内容覆盖，不再频繁构造可变长容器、临时对象和重复映射。resident 单 assignment 场景下的候选提交也走这条薄路径，优先复用 cluster-local 的顺序访问和轻量状态。

这样做的原因是当前 resident 路径已经没有 cluster I/O 的主成本，但仍然保留了较厚的运行时包装层。若不先去掉这些包装，后续即使优化 Stage1 内核，收益也会被外围对象组织和访存跳转吞掉。

备选方案：
- 继续沿用通用 wrapper，只做局部 reserve / inline 优化。
  这个方案改动小，但无法从结构上约束 resident 在线路径的上限，也不利于后续继续做 per-thread scratch 和 cluster-local 提交，因此不采用。

### 决策二：单 bit FastScan 采用“准备阶段分层观测 + 在线阶段收敛字段”的优化策略

实现上，把 `PrepareQueryRotatedInto` 的内部子步骤继续显式区分出来，并重点压缩两块：一是 query 归一化/旋转后用于 Stage1 的中间态组织；二是 LUT 构建。在线 query 阶段保留最少的 `PreparedQuery` 字段，避免为 Stage1 保留过重的长期中间 buffer。需要的 scratch 中间态只允许以局部临时状态存在。

这样做的原因是 perf 已经显示 `PrepareQueryRotatedInto` 与 `BuildFastScanLUT` 都占了稳定热点，而 Stage1 又是整段 probe 中最大的组成部分。这里的优化必须同时关注“算子常数”和“PreparedQuery 的生命周期膨胀”。

备选方案：
- 只优化 `EstimateDistanceFastScan` 内核，不碰 prepare/LUT。
  这个方案能降低部分计算开销，但无法解决 `probe_prepare_ms` 和 Stage1 前置准备过重的问题，因此不采用。

### 决策三：resident + single assignment 下允许采用轻量提交路径，而不是保守的全局重逻辑

实现上，在 resident 且 single assignment 的条件下，候选提交路径可以使用更轻量的组织方式：减少全局去重检查、减少高频小对象分配，并将 cluster 内顺序候选转成更便于后续处理的紧凑批次。这里优化的重点是提交组织成本，不改变候选语义，也不提前引入新的 batch rerank 行为。

这样做的原因是 single assignment 本身已经显著减弱了跨 cluster 重复的概率，继续沿用为多分配路径设计的保守去重和通用提交流程，收益较低但成本真实存在。

备选方案：
- 完全移除去重。
  这个方案风险偏高，因为系统仍需保留对异常路径或未来扩展模式的正确性边界，因此不直接采用，而是限定在 resident + single assignment 的受控轻量路径内弱化成本。

### 决策四：coarse select 单独优化并保持统计独立

实现上，`FindNearestClusters()` 及其等价路径继续保持独立计时，并作为本轮优化对象。目标是避免 centroid ranking、top-n 选择和 query 侧包装继续占据过大的主路径比例。设计上不把 coarse select 与 probe 合并处理，因为两者后续优化方向不同。

本轮 coarse select 的优先顺序固定为两步先行：
- 第一步，给 `FindNearestClusters()` 引入可复用的 coarse scratch，去掉 query 期间的 `std::vector<std::pair<float, uint32_t>>` 一类临时分配与重建。
- 第二步，收紧 coarse score buffer、index/cluster-id materialization 和相关数据布局，减少 centroid score 写入、搬运、partition/sort 前后的 cache 压力。

这两步完成后，不直接默认进入第三步。需要先跑一轮同口径 profiling，判断当前 `coarse_select_ms` 的剩余热点究竟主要落在 centroid 扫描本身，还是落在 `nth_element + sort(top-nprobe)` 这一类 top-n selection 上。只有 profiling 结果明确支持时，才进入第三步，对 top-n selection 策略做进一步重写或替换。

这样做的原因是最新 benchmark 已经明确表明 coarse select 本身接近 0.70ms，和整个 probe 路径同量级。如果不单独处理，它会成为后续所有优化的固定天花板。

备选方案：
- 暂时只做 Stage1，不碰 coarse select。
  这个方案会让后续结果出现“probe 下降了，但 query 总时延不明显”的假象，因此不采用。
- 一开始就直接重写 top-n selection。
  这个方案不够稳，因为现阶段还没有把 coarse select 内部继续拆成 centroid 打分成本和 top-n 选择成本。若过早重写第三步，可能会把实现复杂度加上去，但收益并不集中在真正主热点上，因此不采用。

### 决策五：验收必须绑定同参数 full E2E 与 query-only perf

实现上，这轮变更完成后必须先确认 CPU 空闲，再跑固定参数 full E2E 与同参数 query-only perf；其中 full E2E 用于报告 recall 和端到端时间，query-only perf 用于分析热点迁移。结果需要与本轮优化前的同口径字段直接对照。

这样做的原因是这轮 change 的目的就是缩短 query 主路径。没有同参数复测和 perf，就无法判断热点是否真的从 wrapper / FastScan / coarse select 上迁走。

## Risks / Trade-offs

- [风险] resident 专用薄路径会进一步增加通用路径与 resident 路径之间的实现分叉。  
  → Mitigation：把触发条件严格限制在 `full_preload + use_resident_clusters=1`，并在 spec 里明确这是 resident 专用要求，不外溢到其他路径。

- [风险] 缩减 `PreparedQuery` 生命周期或中间态后，可能引入 Stage1/Stage2 语义不一致。  
  → Mitigation：要求优化后的 prepare 与 Stage1 distance estimate 对参考实现保持等价，并保留细分统计便于定位回归。

- [风险] resident + single assignment 下弱化去重如果边界判断不严，可能在特殊模式下引入重复候选。  
  → Mitigation：将轻量提交路径限制为受控条件，并保留回退到通用路径的能力。

- [风险] coarse select 优化后热点可能转移到新的未观测子步骤。  
  → Mitigation：保持 `coarse_select_ms` 和 probe 子阶段字段稳定输出，必要时继续细拆其内部子步骤。

## Migration Plan

1. 先在 OpenSpec 中落地 resident hotpath、FastScan Stage1 与 benchmark 验收要求。
2. 实现 resident query wrapper / scratch 复用与轻量提交路径，保持现有结果语义不变。
3. 实现 FastScan prepare/LUT/Stage1 的主路径压缩，并保留现有细分统计字段。
4. 实现 coarse select 的前两步：coarse scratch 复用，以及 coarse score buffer / 映射布局收紧。
5. 编译通过后，确认 CPU 空闲，先跑固定参数 full E2E 和同参数 `--query-only 1` profiling。
6. 基于 profiling 结果决定是否进入 coarse select 的第三步，也就是 top-n selection 重写。
7. 若出现 recall 回归或 perf 热点异常，按 resident hotpath / FastScan / coarse select 三段回滚定位。

## Open Questions

- coarse select 在前两步完成后，是否需要继续细拆成 centroid distance 和 top-n selection 两段，以支撑第三步决策。
- resident hotpath 的 scratch 生命周期是按线程持有还是按 scheduler 持有，哪种更便于后续继续优化。
- 单 bit FastScan 的 LUT 构建是否需要在本轮就引入更激进的融合实现，还是先通过字段收敛和内存布局调整拿第一轮收益。
