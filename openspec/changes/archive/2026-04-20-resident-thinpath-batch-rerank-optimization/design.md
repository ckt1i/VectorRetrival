## Context

当前 `bench_e2e` 在 `full_preload + use_resident_clusters=1 + nprobe=64` 下，聚类数据已经常驻内存，但在线查询路径仍然沿用较厚的通用执行框架：查询阶段仍会经过 resident cluster 访问、调度器状态机、逐候选到达即精排、collector 即时维护和最终 payload 补取等多个环节。与此同时，CRC calibration 仍存在于 `bench_e2e` 生命周期中，导致 benchmark 口径与真正在线查询口径混杂。

本次变更需要同时处理三类问题。第一类是 benchmark 口径问题：CRC calibration 应作为离线或 prepare 阶段能力存在，而不是在线查询口径的一部分。第二类是 resident 路径问题：在 resident 模式下，query 无关的 cluster 元数据、解析结果和辅助结构应提前完成，而不是在每个 query 中继续走通用厚路径。第三类是 rerank 执行问题：当前到达即精排的模式不利于缓存局部性、SIMD 批处理和 collector 合并，需要改为先收集候选、再批量 raw fetch 与批量 rerank。

## Goals / Non-Goals

**Goals:**
- 将 CRC calibration 从 `bench_e2e` 在线计时口径中剥离，使 benchmark 的在线结果只反映真实查询路径。
- 为 `full_preload + use_resident_clusters=1` 提供专用 resident thin path，将 query 无关步骤前移到 prepare/init 阶段。
- 引入 batched rerank 执行模型，在不降低 recall 语义的前提下，降低 rerank 阶段的常数开销并为 SIMD 并行化提供执行基础。
- 在第二阶段和第三阶段实现后，分别使用同一条 benchmark 命令和同一组 perf 方法进行验证，保留阶段性性能证据。

**Non-Goals:**
- 不改变 coarse clustering、single assignment 或 probe 分类规则本身。
- 不在本次变更中重写 CRC early stop 判定逻辑，只调整 calibration 的生命周期与加载方式。
- 不引入新的外部依赖或新的 benchmark CLI 形态，除非实现需要增加最小必要参数。
- 不承诺本次变更后直接达到最终目标时延，只要求建立可解释、可继续迭代的性能台阶。

## Decisions

### 决策一：将 CRC calibration 迁移为离线/prepare 工件加载，而不是 `bench_e2e` 运行期行为

实现上，`bench_e2e` 在线运行时必须优先消费索引目录中已有的 CRC 参数或等价工件；若工件缺失，则 benchmark 应明确失败或走非在线口径，而不是在同一轮 benchmark 中回退到运行期重新 calibration。

这样做的原因是 CRC calibration 的职责是“定标”，不是“查询”。把它留在 benchmark 生命周期里会污染 perf 热点、混淆准备阶段与在线阶段边界，也会让不同 query 数量下的 benchmark wall time 失真。

备选方案：
- 保留现有 fallback inline calibration，只在结果展示时不计入 `avg_query_ms`。
  这个方案仍会污染 perf、污染整个 benchmark 运行路径，而且难以约束后续实验口径，因此不采用。

### 决策二：resident 模式引入专用 thin path，而不是继续复用通用调度框架

实现上，针对 `clu-read-mode=full_preload` 且 `use-resident-clusters=1` 的组合，增加 resident 专用查询路径。prepare/init 阶段负责构建 query 无关的 resident cluster view、地址/元信息映射、解析结果和必要的辅助结构；在线 query 阶段只保留 query 相关步骤，例如 query 准备、阈值判断、实际 probe、候选生成、rerank 与最终 payload fetch。

这样做的原因是 resident 模式已经不需要 window 读、cluster 等待与通用 completion 驱动的很多中间层。如果继续强行复用通用路径，虽然行为兼容，但会保留大量在 resident 模式下没有价值的状态机与抽象开销。

备选方案：
- 只在现有通用路径上做局部 if/else 优化。
  这个方案改动小，但最终会在 resident 模式里保留大量遗留逻辑，收益上限较低，不利于后续继续压缩 query 常数项，因此不采用。

### 决策三：将 rerank 从 arrival-driven 改为 batch-driven

实现上，probe 阶段先输出候选地址或轻量候选描述，在线后段按 cluster 或按 query 统一收集候选，再执行 grouped raw-vector fetch、批量距离计算和批量 top-k merge。payload fetch 继续保留在最终结果阶段，避免对短命候选过早进行 payload 处理。

这样做的原因是到达即精排模式会带来三类问题：原始向量读取离散、`L2Sqr` 无法形成稳定批处理、collector 被高频小粒度调用。批量 rerank 可以改善缓存局部性，为 SIMD 批并行化铺路，并减少 collector 和 payload cache 的扰动。

备选方案：
- 保持单候选即时 rerank，只对 `L2Sqr` 内核本身做 SIMD 微优化。
  这个方案只能优化算子常数，无法解决执行方式碎片化的问题，收益上限较低，因此不采用。

### 决策四：阶段化验收必须成为设计的一部分

本次变更分两阶段验收。第二阶段完成 resident thin path 后，必须使用同一条 benchmark 命令先测速，再做 perf 采样，记录当下热点。第三阶段完成 batched rerank 后，必须重复同样的测速与 perf 流程。

这样做的原因是这次变更是结构性优化，不是单点 patch。没有阶段化速度结果和 perf 热点，后续无法判断收益来自哪一层，也无法避免多个优化相互掩盖。

## Risks / Trade-offs

- [风险] resident thin path 与通用路径并存后，逻辑分叉增多，后续维护成本上升。  
  → Mitigation：将 thin path 触发条件严格限制为 `full_preload + resident`，并把通用/专用路径的职责边界写入 spec 与任务清单。

- [风险] batched rerank 可能改变候选进入 collector 的时机，若处理不当会影响 recall 或结果顺序。  
  → Mitigation：明确要求保持当前 recall 语义和结果排序一致，并在阶段三验收中对比优化前后的 recall 结果。

- [风险] CRC calibration 工件若缺失，benchmark 可能无法直接运行。  
  → Mitigation：将“必须使用预计算工件”的要求写入 benchmark spec，并在实现中提供明确错误信息与准备步骤说明。

- [风险] thin path 把大量步骤前移到 prepare 阶段后，启动成本会增加。  
  → Mitigation：本次优化目标是降低在线 query 时延，允许 prepare 成本上升，但要求 benchmark 明确区分 prepare 与在线阶段。

## 执行策略补充

- probe 阶段继续沿用现有 SafeIn 和 Uncertain 各自的提交与解码方式，不在本次变更中重写这两条路径。
- 每 probe 完一个 cluster，系统沿用现有路径提交该 cluster 候选对应的 raw vector prefetch。
- 这里的 raw vector prefetch 明确指按 cluster 分批从磁盘向原始向量内存池发起的预读，它的职责是把后续可能参与 rerank 的原始向量提前搬运到内存池中。
- 对于被判定为 SafeIn 的少量候选，系统沿用现有路径提前提交对应 payload prefetch。
- 待全部目标 cluster probe 完成后，系统统一整理候选集合，并从原始向量内存池读取已预取好的原始向量执行一轮 batch rerank。
- 这里的内存池读取明确指从原始向量内存池批量读取已经预取完成的原始向量，组织 batch 输入并执行统一 rerank；它不再属于磁盘预读阶段。
- 最终结果阶段仅对未命中 SafeIn payload 预取的结果执行 remaining payload fetch。

## 统计口径补充

- `probe_ms`：probe / classify / threshold 判断的 CPU 时间。
- `prefetch_submit_ms`：probe 过程中提交 raw vector prefetch 和 SafeIn payload prefetch 的时间。
- `prefetch_wait_ms`：为确保 batch rerank 前数据齐备而发生的等待时间。
- `safein_payload_prefetch_ms`：SafeIn 候选提前预取 payload 的时间。
- `candidate_collect_ms`：全部目标 cluster probe 完成后，统一整理候选集合的时间。
- `pool_vector_read_ms`：从原始向量内存池读取已预取原始向量、组织 batch 输入的时间；该字段与 probe 阶段的磁盘 prefetch 预读分开统计。
- `rerank_compute_ms`：统一 batch rerank 和 top-k merge 的计算时间。
- `remaining_payload_fetch_ms`：最终结果中仍未命中 SafeIn payload 预取的剩余 payload 补取时间。
- `e2e_ms`：完整在线 query 时延。
- `num_candidates_buffered`：进入候选缓冲区、等待统一 batch rerank 的候选数量。
- `num_candidates_reranked`：真正参与 batch rerank 的候选数量。
- `num_safein_payload_prefetched`：在 SafeIn 阶段已提前预取 payload 的候选数量。
- `num_remaining_payload_fetches`：最终结果中仍需执行 remaining payload fetch 的数量。
