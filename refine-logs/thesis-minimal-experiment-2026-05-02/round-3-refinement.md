# Round 3 Refinement: User Argue + Code-Grounded Revision

## Problem Anchor

- Bottom-line problem: 在本科毕业论文时间约束下，将当前 VectorRetrival/BoundFetch 代码收敛为一版最小但完整、可复现、可写作的实验闭环，证明集成式向量检索与 payload 读取协同设计在真实检索和合成 payload 场景中优于拆分式 baseline。
- Must-solve bottleneck: 旧方向同时展开 CRC 早停、RAIR/重复聚类、CPU 热路径优化和大 baseline 矩阵，实验面过宽；当前必须缩小到单聚类划分、无 CRC 早停、有限数据集、有限 baseline 的主实验与 ablation，使论文能按期完成。
- Non-goals: 不证明 ConANN CRC 早停；不做 RAIR 或重复聚类；不做完整 `search x storage x dataset` 笛卡尔矩阵；不追求击败所有图索引或商业向量数据库；不新增复杂学习模块。
- Constraints: 数据集只考虑 `coco_100k`、`deep1m_synth`、`msmarco_passage`；其中 `deep1m_synth` 仍需生成，当前 `/home/zcq/VDB/data/formal_baselines` 下看到的是 `deep8m_synth`；baseline 只考虑 `IVF+PQ/IVF+RaBitQ` 与 `FlatStor/Lance` 的四种组合；当前代码执行时固定 `--crc 0`、`--assignment-mode single`，不启用 RAIR/重复聚类；复用 `/home/zcq/VDB/baselines` 现有脚本和结果格式；论文写作参考 VLDB/PVLDB 的系统论文结构。
- Success condition: 产出一套可执行实验矩阵、主实验图表、关键 ablation、方法叙事、创新点和毕业论文大纲；即使最终只完成 COCO 100K 与一个扩展数据集，也能形成自洽论文。

## Anchor Check

- Original bottleneck: 需要从过宽的 CRC/RAIR/全 baseline study 中收敛，但不能把实验收敛到只有一个数据集而丧失普适性。
- Why the revised method still addresses it: 新方案保留单聚类、无 CRC 早停、无 RAIR 的方法边界，但主实验恢复到 `COCO100K + MS MARCO` 两个真实 workload，`deep1m_synth` 作为第三数据集增强项。
- Reviewer suggestions rejected as drift: Round 1 reviewer 建议把主线压成 COCO-only，这对“本科按期完成”有利，但会削弱学术结论的普适性；本轮不再接受 COCO-only 作为最终主线。

## Simplicity Check

- Dominant contribution after revision: `BoundFetch-Guarded`，即 RaBitQ error-bound 三分类驱动的 guarded fetch/rerank pipeline。
- Components removed or merged: 仍不纳入 CRC 早停、RAIR、重复聚类、DiskANN 大调参或额外训练模块。
- Reviewer suggestions rejected as unnecessary simplification: 固定 top-`C` 候选预算不是当前代码真实机制，已移除。
- Why the remaining mechanism is still small: 它复用现有 `ClusterProber`、`AsyncIOSink`、`OverlapScheduler` 与 baseline 脚本，不新增模型或索引结构；扩展的是实验覆盖面和方法表述准确性。

## Code-Grounded Corrections

1. `include/vdb/index/cluster_prober.h` 已定义 `CandidateClass { SafeIn, Uncertain }`，SafeOut 在 `ClusterProber` 内部消费并计入 stats。
2. `src/index/cluster_prober.cpp` 的 Stage 1/Stage 2 会根据 RaBitQ 估计距离和 margin 做 SafeOut/SafeIn/Uncertain 分类。
3. `include/vdb/index/conann.h` 中 `ClassifyAdaptive` 当前实现为：
   - SafeOut: `approx_dist > dynamic_d_k + 2 * margin`
   - SafeIn: `approx_dist < d_k_ - 2 * margin`
   - Uncertain: otherwise
4. 因此论文抽象可以写成 “围绕当前/校准 top-k 半径的 bound interval”，但实现细节应准确说明：SafeOut 使用动态 estimate heap 的 `dynamic_d_k`，SafeIn 使用校准的全局 `d_k_` 保守判定。
5. `AsyncIOSink` 对 SafeIn 且 payload 小于 `safein_all_threshold` 的候选生成 `VEC_ALL` read plan；其他候选生成 `VEC_ONLY` read plan。
6. `OverlapScheduler` 维护 `pending_all_plans_` 和 `pending_vec_only_plans_`，由 `submit_batch_size`、`io_queue_depth`、`cluster_submit_reserve`、`enable_online_submit_tuning`、`refill_threshold/refill_count` 等控制动态提交。

## Changes Made

### 1. 数据集主线恢复到至少两个真实数据集

- User said: 只有 COCO100K 会缺乏普适性，主流会议需要多个数据集和多个 baseline。
- Action: Mandatory 主实验改为 `COCO100K + MS MARCO Passage`；`deep1m_synth` 作为 strong target / optional 第三数据集。
- Reasoning: COCO 覆盖图文跨模态，MS MARCO 覆盖文本检索；两者比 COCO-only 更能支撑“payload-rich embedding serving”结论。
- Impact: 论文结论更 solid，但执行计划需分层：先打通 COCO，再复制到 MS MARCO，Deep1M 作为增强。

### 2. 方法从 fixed top-C 改成 bound-guided triage

- User said: 候选预算不是固定 top-k，而是利用 RaBitQ error bound 划分 SafeIn/SafeOut/Uncertain。
- Action: 删除 fixed top-C 作为主机制，改为 `SafeIn -> read vector+payload`、`SafeOut -> skip`、`Uncertain -> read vector first, payload later if final top-k`。
- Reasoning: 这才是现有代码和方法创新的真实核心。
- Impact: 贡献从“执行计划整合”进一步具体化为“bound-guided selective fetch”。

### 3. 动态 fetch 调度进入方法贡献

- User said: 异步 fetch 根据 io_uring 堆积任务和 probe 数量动态调整，现有代码已有实现。
- Action: 方法章节新增 dynamic submit policy：pending request pressure、submit batch、tail flush、cluster refill、online tuning。
- Reasoning: 这是系统贡献的重要组成部分，不能只写成普通 overlap。
- Impact: ablation 需要从简单 overlap on/off 改成 guard triage 与 dynamic scheduling 两组机制验证。

## Revised Proposal

# Research Proposal: BoundFetch-Guarded 本科论文最小完整实验方案

## Problem Anchor

- Bottom-line problem: 在本科毕业论文时间约束下，将当前 VectorRetrival/BoundFetch 代码收敛为一版最小但完整、可复现、可写作的实验闭环，证明集成式向量检索与 payload 读取协同设计在真实检索和合成 payload 场景中优于拆分式 baseline。
- Must-solve bottleneck: 旧方向同时展开 CRC 早停、RAIR/重复聚类、CPU 热路径优化和大 baseline 矩阵，实验面过宽；当前必须缩小到单聚类划分、无 CRC 早停、有限数据集、有限 baseline 的主实验与 ablation，使论文能按期完成。
- Non-goals: 不证明 ConANN CRC 早停；不做 RAIR 或重复聚类；不做完整 `search x storage x dataset` 笛卡尔矩阵；不追求击败所有图索引或商业向量数据库；不新增复杂学习模块。
- Constraints: 主实验至少完成 `coco_100k` 与 `msmarco_passage`；`deep1m_synth` 作为第三数据集增强项，若时间不足则做合成 payload fallback；baseline 只考虑 `IVF+PQ/IVF+RaBitQ` 与 `FlatStor/Lance` 的四种组合；当前代码执行时固定 `--crc 0`、`--assignment-mode single`，不启用 RAIR/重复聚类；复用 `/home/zcq/VDB/baselines` 现有脚本和结果格式；论文写作参考 VLDB/PVLDB 的系统论文结构。
- Success condition: 产出 COCO100K 与 MS MARCO 上的主实验表和 recall/quality-latency 曲线，完成关键机制 ablation，并给出可写进毕业论文的方法、创新点和章节大纲；Deep1M_synth 若完成则增强普适性，否则不阻塞主论文。

## Technical Gap

拆分式 baseline 通常把服务路径拆成两个阶段：先用 `IVF+PQ` 或 `IVF+RaBitQ` 找候选，再把最终 id 交给 `FlatStor/Lance` 读取 payload。这种 pipeline 的问题不是单个 ANN kernel 不够快，而是候选确定、原向量读取、payload 读取与 rerank 之间缺少联动：

1. 对明显不可能进入 top-k 的候选，拆分式 pipeline 缺少基于误差界的早期跳过。
2. 对高置信会进入 top-k 的候选，拆分式 pipeline 通常等 ANN 阶段结束后才读取 payload。
3. 对 uncertain 候选，系统需要先读原向量完成 rerank，但不应提前读取所有 payload。
4. 当候选读取和 cluster probe 同时发生时，固定批量提交容易导致 io_uring 队列压力与 CPU probe 进度不匹配。

当前代码已经具备上述机制的雏形：`ClusterProber` 做 SafeIn/SafeOut/Uncertain 三分类，`AsyncIOSink` 将 SafeIn 映射为 `VEC_ALL`、Uncertain 映射为 `VEC_ONLY`，`OverlapScheduler` 根据 pending request、probe 进度和 queue depth 动态提交 I/O。论文应把这些写成核心方法，而不是用 fixed top-C 误抽象。

## Method Thesis

- One-sentence thesis: BoundFetch-Guarded 利用 RaBitQ 估计误差界将候选划分为 SafeIn、SafeOut 与 Uncertain，并用动态 io_uring 调度分别执行 payload+vector 预取、跳过和 vector-only rerank，从而在多数据集 matched-quality 条件下降低 payload-rich retrieval 的 E2E latency。
- Why this is the smallest adequate intervention: 它不新增索引族、训练模块、CRC 早停、RAIR 或重复聚类，只把现有 single-assignment IVF + RaBitQ + async I/O 路径组织成一个可解释、可验证的 guarded serving pipeline。
- Why this route is timely: 现代 embedding retrieval 的用户可感知路径是 `retrieve -> fetch -> rerank -> return object`；系统优化应围绕对象返回的 E2E latency 与 tail latency，而不是只比较 ANN kernel。

## Contribution Focus

- Dominant contribution: `BoundFetch-Guarded`，一个 RaBitQ error-bound guided 的 selective fetch/rerank pipeline，在 single-assignment IVF 中用 SafeIn/SafeOut/Uncertain 决定 payload 与原向量读取策略。
- Supporting contribution: 一个覆盖 COCO100K 与 MS MARCO 的 matched-quality E2E benchmark protocol，并以 Deep1M_synth payload sensitivity 作为可选第三数据集增强。
- Explicit non-contributions: CRC early stop、RAIR、duplicate clustering、encoder 训练、DiskANN/HNSW/ScaNN 全量对比、完整商业向量数据库评测。

## Proposed Method

### Complexity Budget

- Frozen / reused backbone: Faiss coarse kmeans、single assignment IVF、RaBitQ estimator、`ClusterProber`、`OverlapScheduler`、FlatStor/Lance baseline runners。
- New trainable components: 无。
- New conceptual component: bound-guided guarded fetch policy 的论文表述与 ablation。
- Tempting additions intentionally not used: CRC stop、RAIR、多重 assignment、learned probe policy、额外 reranker、Parquet、DiskANN 主线。

### Core Mechanism: Bound-Guided Candidate Triage

对每个 query，系统先选择 `nprobe` 个 IVF cluster，并用 RaBitQ 对 cluster 内候选估计距离。设当前 top-k 半径为 `d_k`，RaBitQ 误差界在实现中体现为 per query/cluster margin。候选被划分为三类：

- SafeIn: 估计距离足够小，即使考虑误差仍应进入 top-k。系统提交 `VEC_ALL`，直接读取原向量和 payload。
- SafeOut: 估计距离足够大，即使考虑误差也不可能进入 top-k。系统直接跳过，不提交 data read。
- Uncertain: 落在边界区间内。系统提交 `VEC_ONLY`，先读取原向量用于 exact rerank；只有最终进入 top-k 时才补读 payload。

实现注意：

- 论文可用 `d_k ± 2ε` 解释核心直觉。
- 代码中的实际 margin 是 per query/cluster 动态量；SafeOut 使用 `dynamic_d_k + 2 * margin`，SafeIn 当前使用校准全局 `d_k_ - 2 * margin`，因此论文应写成“保守 SafeIn + 动态 SafeOut”的实现版本。
- `safein_all_threshold` 控制 SafeIn 是否直接读完整 payload；超过阈值的记录可退化为 vector-only，以避免大 payload 过早放大 I/O。

### Dynamic Fetch Scheduling

BoundFetch-Guarded 不把所有 read plan 立即提交，而是维护两个队列：

- `pending_all_plans_`: SafeIn 的 vector+payload read。
- `pending_vec_only_plans_`: Uncertain 的 vector-only read。

调度策略根据以下信号动态 flush：

- pending request 数量是否超过 hard threshold。
- pending request 是否达到 `submit_batch_size` 或 online-tuned threshold。
- 当前 probe 了多少 cluster，是否进入 tail flush。
- `io_queue_depth` 中需要为 cluster read 保留的 slot 数。
- `refill_threshold/refill_count` 是否需要继续补充 cluster prefetch。
- shared vs isolated submission mode 下 cluster/data reader 是否需要分别 submit。

这使系统可以在 probe CPU 工作、cluster block prefetch、candidate vector read 与 payload read 之间做更细粒度重叠。

### System Graph

```text
offline:
  dataset embeddings + payload
    -> Faiss kmeans
    -> single cluster assignment
    -> per-cluster RaBitQ / address layout
    -> FlatStor and Lance payload exports

online:
  query embedding
    -> top-nprobe clusters
    -> RaBitQ estimate per candidate
    -> SafeIn / SafeOut / Uncertain triage
       SafeIn    -> read vector + payload
       SafeOut   -> skip
       Uncertain -> read vector only
    -> exact rerank on fetched vectors
    -> fetch missing payload for final top-k
    -> return top-k payload-bearing results
```

## Experiment Plan

### Main Datasets

主实验必须覆盖两个真实 workload：

| Dataset | Role | Required? | Notes |
|---------|------|-----------|-------|
| `coco_100k` | 图文跨模态检索，当前代码与脚本最成熟 | Yes | 第一优先级，先打通完整矩阵 |
| `msmarco_passage` | 文本 dense retrieval，支撑普适性 | Yes | 第二优先级，至少完成 FlatStor，目标完成四组合 |
| `deep1m_synth` | 标准向量 + 合成 payload size 控制变量 | Strong target | 时间不足时可降级为 appendix 或 COCO synthetic padding |

### Main Baselines

主 baseline 恢复为用户原始约束中的四种组合：

1. `IVF+PQ+FlatStor`
2. `IVF+RaBitQ+FlatStor`
3. `IVF+PQ+Lance`
4. `IVF+RaBitQ+Lance`

执行分层：

- Tier 0: COCO100K 上四组合全部完成。
- Tier 1: MS MARCO 上四组合全部完成；若 Lance runner 阻塞，先完成两种 FlatStor 组合并在报告中标为 in-progress risk。
- Tier 2: Deep1M_synth 上优先 FlatStor 两组合，Lance 可选。

### Metrics

- Retrieval quality: `recall@10`；MS MARCO 若 qrels 可用，补 `MRR@10` / `nDCG@10`。
- E2E serving: avg / p50 / p95 / p99 latency。
- I/O behavior: total reads、SafeIn/SafeOut/Uncertain counts、vector-only reads、payload-prefetched reads、missing payload fetches、bytes read。
- System breakdown: coarse scoring、probe stage1/stage2、submit prep、io wait、rerank、fetch missing payload。
- Resource metrics: index size、payload store size、build time。

### Matched-Quality Rule

主表不选择任意最快点。对每个 dataset，先确定所有主线系统共同可达的最高质量区间 `Q*`：

- COCO: common `recall@10` target。
- MS MARCO: common `MRR@10` 或 `recall@10` target，取决于 qrels 是否可用。

每个系统报告达到 `Q*` 的最低 E2E latency，并同时给出完整 quality-latency curve。

## Claim-Driven Validation

### Claim 1: 方法在多个真实数据集上优于拆分式 baseline

- Minimal experiment: COCO100K 与 MS MARCO 上比较 BoundFetch-Guarded 与四种 baseline。
- Metric: matched-quality avg/p95/p99 E2E latency。
- Expected evidence: BoundFetch-Guarded 在两个 workload 上都低于拆分式 `IVF+PQ/RaBitQ + FlatStor/Lance` 的 E2E latency。

### Claim 2: 收益来自 bound-guided triage，而不是只来自 RaBitQ scorer

- Minimal experiment: 与 `IVF+RaBitQ+FlatStor` 对齐量化器；再做 triage ablation。
- Ablations:
  - disable SafeIn payload prefetch: SafeIn 也只读 vector，最后补 payload。
  - disable SafeOut pruning: SafeOut 不跳过，退化为读更多 vector。
  - Uncertain eager payload: Uncertain 也读 payload，观察 I/O 放大。
- Metric: SafeIn/SafeOut/Uncertain counts、bytes read、payload reads、E2E latency。
- Expected evidence: 正常 guarded policy 在 read amplification 和 latency 上最优。

### Claim 3: 动态 fetch 调度降低队列压力和 tail latency

- Minimal experiment: 固定 triage policy，对比 dynamic submit vs fixed submit / serial fetch。
- Ablations:
  - online submit tuning on/off。
  - fixed submit batch sizes。
  - serial fetch after probe。
- Metric: p95/p99 latency、io wait、submit calls、queue flush counts、tail flush counts。
- Expected evidence: 动态调度降低 tail latency，尤其在候选数量随 query 波动时更明显。

### Claim 4: payload size 越大，selective fetch 越重要

- Minimal experiment: Deep1M_synth 或 fallback synthetic padding 上扫 payload size。
- Metric: payload size vs speedup / bytes read / p99 latency。
- Expected evidence: payload 增大时，Uncertain 延迟 payload 与 SafeOut skip 的收益更明显。

## Fast Execution Plan

### Phase A: COCO100K 完整闭环

1. 跑 BoundFetch-Guarded 主路径。
2. 跑四个 baseline 组合。
3. 生成 quality-latency curve、matched-quality 主表和 latency breakdown。
4. 输出 SafeIn/SafeOut/Uncertain 统计，确认 triage 工作。

### Phase B: MS MARCO 主实验

1. 确认 embedding、query、qrels 或 ANN ground truth。
2. 复用 COCO 的参数结构，先完成 FlatStor 两组合。
3. 补 Lance 两组合。
4. 生成与 COCO 对齐的主表与曲线。

### Phase C: Mechanism Ablation

1. Triage ablation: SafeIn / SafeOut / Uncertain 三项。
2. Scheduling ablation: dynamic vs fixed / serial。
3. 只在 COCO 上完整做；MS MARCO 可做一组关键 operating point。

### Phase D: Deep1M_synth / Payload Sensitivity

1. 若 `deep1m_synth` 能及时生成，跑 2-3 个 payload size。
2. 若不能，使用 COCO synthetic payload padding 做 fallback。
3. 该阶段增强论文普适性，但不阻塞 COCO + MS MARCO 主结论。

## Paper-Writable Contributions

1. Bound-guided selective fetch: 利用 RaBitQ error bound 将候选划分为 SafeIn/SafeOut/Uncertain，并为三类候选选择不同读取策略。
2. Dynamic async scheduling: 根据 pending I/O、probe 进度和 queue capacity 动态提交 vector/payload read，使 probe、rerank 和 fetch 重叠。
3. Multi-workload E2E validation: 在 COCO100K 与 MS MARCO 上以 matched-quality E2E latency 验证，并用 Deep1M_synth 或 payload padding 检查 payload size sensitivity。

## VLDB/PVLDB-Style Thesis Outline

### 第 1 章 引言

- dense retrieval serving 的 `retrieve -> fetch -> rerank` E2E bottleneck。
- 为什么拆分式 ANN + payload fetch 不够。
- 本文贡献与实验覆盖。

### 第 2 章 背景与相关工作

- IVF、PQ、RaBitQ。
- 误差界驱动的候选过滤。
- payload-aware vector retrieval 与 Lance/FlatStor。

### 第 3 章 问题定义与动机

- E2E latency、matched quality、payload fetch amplification。
- COCO 与 MS MARCO 的 workload 差异。
- SafeIn/SafeOut/Uncertain 的动机统计。

### 第 4 章 BoundFetch-Guarded 方法

- single-assignment IVF layout。
- RaBitQ bound-guided triage。
- SafeIn/SafeOut/Uncertain 读取策略。
- dynamic io_uring fetch scheduling。
- 不启用 CRC early stop / RAIR 的边界。

### 第 5 章 实现

- `ClusterProber`、`AsyncIOSink`、`OverlapScheduler`。
- 关键参数：`nprobe`、`safein_all_threshold`、`submit_batch_size`、`io_queue_depth`、`refill_threshold/refill_count`。
- 数据和 baseline 复用路径。

### 第 6 章 实验设置

- 数据集：COCO100K、MS MARCO、Deep1M_synth optional。
- Baselines：四组合。
- Metrics：quality、avg/p95/p99 E2E、bytes read、breakdown。
- matched-quality 规则。

### 第 7 章 实验结果

- 主实验：COCO + MS MARCO。
- baseline 对比：四组合。
- 机制 ablation：triage 与 dynamic scheduling。
- payload sensitivity：Deep1M_synth / fallback。

### 第 8 章 讨论与局限

- 为什么没有 CRC/RAIR。
- MS MARCO qrels 或 embedding 准备风险。
- Lance runner 复用风险。
- Deep1M_synth 作为第三数据集的时间风险。

### 第 9 章 结论

- 总结 bound-guided selective fetch 与动态调度的 E2E 收益。

## Remaining Execution Risks

- `msmarco_passage` 需要确认 embedding、query 与 qrels/ground truth 完整性。
- Lance 四组合是否已完全可复用仍需执行验证。
- 当前代码 SafeIn 使用静态 `d_k_`，SafeOut 使用动态 `dynamic_d_k`；论文必须按实现写，不能声称 SafeIn/SafeOut 都完全由同一个当前 top-k 半径决定。
- 如果要严格复现用户描述的 “SafeIn/SafeOut 均围绕当前 `d_k`” 版本，需要额外代码修改和 correctness 验证；不建议作为本科论文主线。
