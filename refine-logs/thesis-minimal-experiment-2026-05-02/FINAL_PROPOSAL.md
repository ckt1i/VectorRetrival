# Research Proposal: BoundFetch-Guarded 本科论文最小完整实验方案

## Final Thesis

`BoundFetch-Guarded` 利用 RaBitQ 估计误差界将候选划分为 SafeIn、SafeOut 与 Uncertain，并用动态 io_uring 调度分别执行 payload+vector 预取、跳过和 vector-only rerank，从而在 COCO100K 与 MS MARCO 等多 workload 的 matched-quality 条件下降低 payload-rich retrieval 的 E2E latency。

## Problem Anchor

- Bottom-line problem: 在本科毕业论文时间约束下，将当前 VectorRetrival/BoundFetch 代码收敛为一版最小但完整、可复现、可写作的实验闭环，证明集成式向量检索与 payload 读取协同设计在真实检索和合成 payload 场景中优于拆分式 baseline。
- Must-solve bottleneck: 旧方向同时展开 CRC 早停、RAIR/重复聚类、CPU 热路径优化和大 baseline 矩阵，实验面过宽；当前必须缩小到单聚类划分、无 CRC 早停、有限数据集、有限 baseline 的主实验与 ablation，使论文能按期完成。
- Non-goals: 不证明 ConANN CRC 早停；不做 RAIR 或重复聚类；不做完整 `search x storage x dataset` 笛卡尔矩阵；不追求击败所有图索引或商业向量数据库；不新增复杂学习模块。
- Constraints: 主实验至少完成 `coco_100k` 与 `msmarco_passage`；`deep1m_synth` 作为第三数据集增强项，若时间不足则做合成 payload fallback；baseline 只考虑 `IVF+PQ/IVF+RaBitQ` 与 `FlatStor/Lance` 的四种组合；当前代码执行时固定 `--crc 0`、`--assignment-mode single`，不启用 RAIR/重复聚类；复用 `/home/zcq/VDB/baselines` 现有脚本和结果格式；论文写作参考 VLDB/PVLDB 的系统论文结构。
- Success condition: 产出 COCO100K 与 MS MARCO 上的主实验表和 recall/quality-latency 曲线，完成关键机制 ablation，并给出可写进毕业论文的方法、创新点和章节大纲；Deep1M_synth 若完成则增强普适性，否则不阻塞主论文。

## Core Method

### Bound-Guided Candidate Triage

系统先 probe IVF clusters，并用 RaBitQ 估计候选距离。根据估计距离与误差界，候选分为三类：

- SafeIn: 估计距离足够小，保守判断应进入 top-k，直接读取原向量和 payload。
- SafeOut: 估计距离足够大，判断不可能进入 top-k，直接跳过。
- Uncertain: 落在边界区间，先读取原向量做 exact rerank；只有最终进入 top-k 时才补读 payload。

实现口径：

- 论文可以用 `d_k ± 2ε` 解释三分类直觉。
- 当前代码中 SafeOut 使用动态 estimate heap 的 `dynamic_d_k + 2 * margin`。
- 当前代码中 SafeIn 使用校准全局 `d_k_ - 2 * margin`，因此论文应写成“保守 SafeIn + 动态 SafeOut”，不要声称两者完全围绕同一个动态当前 top-k 半径。
- SafeIn 只有在 payload size 不超过 `safein_all_threshold` 时直接走 `VEC_ALL`；否则退化为 vector-only，避免大 payload 过早放大 I/O。

### Dynamic Fetch Scheduling

BoundFetch-Guarded 维护两个读取队列：

- `pending_all_plans_`: SafeIn 的 vector+payload read。
- `pending_vec_only_plans_`: Uncertain 的 vector-only read。

调度器根据 pending request 数量、`submit_batch_size`、`io_queue_depth`、cluster read reserve、probe 进度、tail flush 条件和 online submit tuning 动态决定何时向 io_uring submit。这个机制把 cluster probe、candidate vector read、payload read 和 rerank 尽量重叠，而不是在 probe 完成后串行 fetch。

## Main Experiment Matrix

### Datasets

| Dataset | Role | Priority |
|---------|------|----------|
| `coco_100k` | 图文跨模态检索，现有资产最成熟 | Mandatory |
| `msmarco_passage` | 文本 dense retrieval，补足普适性 | Mandatory |
| `deep1m_synth` | 合成 payload size 控制变量 | Strong target / appendix |

### Baselines

主 baseline 使用四种组合：

1. `IVF+PQ+FlatStor`
2. `IVF+RaBitQ+FlatStor`
3. `IVF+PQ+Lance`
4. `IVF+RaBitQ+Lance`

执行分层：

- Tier 0: COCO100K 上完成四组合。
- Tier 1: MS MARCO 上完成四组合；如果 Lance runner 阻塞，先完成 FlatStor 两组合并记录风险。
- Tier 2: Deep1M_synth 优先完成 FlatStor 两组合，Lance 可选。

## Metrics and Matching Rule

- 主 operating point: `topk=10`，报告 `recall@10` + avg/p50/p95/p99 E2E latency；MS MARCO 若 qrels 可用，主质量指标使用 `MRR@10` / `nDCG@10`。
- 补充 operating point: `topk=20`，用于主文补充表或补充曲线，验证更大返回集合下 selective fetch 是否仍有效。
- Appendix stress point: `topk=50`，只在 COCO 或 Deep1M_synth 上做压力测试，观察 rerank 和 payload fetch 放大；不进入所有 dataset x backend 主矩阵。
- 当前实现注意：`bench_e2e` 支持 `--topk`，但内置 recall 输出当前固定为 `@1/@5/@10`。若正式报告 `recall@20/@50`，需要扩展 benchmark 输出或基于 per-query predictions 后处理。
- MS MARCO: 若 qrels 不完整，先报告 ANN `recall@10/@20` 与 E2E latency，把 `MRR@10/nDCG@10` 标为待补。
- 系统指标：SafeIn/SafeOut/Uncertain counts、bytes read、vector-only reads、payload-prefetched reads、missing payload fetches、submit calls、queue flushes、latency breakdown。
- 主表采用 matched-quality：对每个 dataset 取所有主线系统共同可达的最高质量区间 `Q*`，每个系统报告达到 `Q*` 的最低 E2E latency。

## Claim-Driven Validation

### Claim 1: 多真实数据集上优于拆分式 baseline

- Experiment: COCO100K 与 MS MARCO 上比较 BoundFetch-Guarded 与四种 baseline。
- Metric: matched-quality avg/p95/p99 E2E latency。
- Expected evidence: BoundFetch-Guarded 在两个 workload 上都降低 E2E latency。

### Claim 2: 收益来自 bound-guided triage

- Experiment: 对齐 `IVF+RaBitQ` 量化器，做 triage ablation。
- Ablations:
  - SafeOut pruning off: SafeOut 不跳过，进入 vector-only rerank，衡量 rerank 剪枝强度。
  - SafeIn prefetch off: SafeIn 不直接读 payload，只读 vector，衡量高置信 payload prefetch 的收益。
  - Uncertain eager payload: Uncertain 也提前读 payload，衡量错误预取和 bytes read 放大。
  - Boundary sensitivity: 调整 margin / epsilon scale 或选择不同 calibration percentile，观察 SafeIn/SafeOut/Uncertain 比例、recall 和 latency 的变化。
  - Stage2 reclassification on/off: 若实现开关可用，评估 ExRaBitQ Stage2 对 uncertain 收缩和 rerank 数量的贡献。
- Metric: SafeIn/SafeOut/Uncertain ratio、s2_safe_in/s2_safe_out、reranked candidates、payload-prefetched reads、missing payload fetches、bytes read、recall/quality、E2E latency。

### Claim 3: 动态 fetch 调度降低 tail latency

- Experiment: dynamic submit vs fixed submit / serial fetch。
- Ablations:
  - online submit tuning on/off。
  - fixed `submit_batch_size` sweep。
  - serial fetch after probe。
  - `safein_all_threshold` sweep，观察 SafeIn payload prefetch 对 queue pressure 的影响。
- Metric: p95/p99 latency、io wait、uring prep/submit、submit calls、flush counts、tail flush counts、pending request count、bytes read。

### Claim 3b: top-k 放大下 selective fetch 仍保持稳定

- Experiment: `topk=10` 主结果，`topk=20` 主文补充，`topk=50` appendix stress。
- Metric: quality-latency curve、reranked candidates、payload reads、p99 latency。
- Expected evidence: top-k 增大时，所有系统 latency 上升，但 BoundFetch-Guarded 的 selective fetch 仍能控制 read amplification。

### Claim 4: payload size 越大，selective fetch 越重要

- Experiment: Deep1M_synth 或 COCO synthetic payload padding。
- Metric: payload size vs speedup / bytes read / p99 latency。

## Fast Execution Plan

1. COCO100K：先跑 BoundFetch-Guarded 和四 baseline，生成主表、曲线和 triage stats。
2. MS MARCO：确认 embedding/query/qrels，复用同样参数结构，先 FlatStor 后 Lance。
3. top-k 扩展：主矩阵固定 `topk=10`；COCO 上补 `topk=20`，时间允许再做 `topk=50` stress。
4. Ablation：COCO 上完整做 triage、boundary、scheduling；MS MARCO 做关键 operating point。
5. Deep1M_synth：有时间则做第三数据集和 payload size sensitivity；否则用 COCO synthetic payload padding fallback。

## Paper-Writable Contributions

1. Bound-guided selective fetch: 用 RaBitQ error bound 将候选分成 SafeIn/SafeOut/Uncertain，并为三类候选选择不同读取策略。
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
- SafeIn/SafeOut/Uncertain 的动机统计。
- 拆分式 pipeline 在 rerank 和 payload fetch 上的串行等待。

### 第 4 章 BoundFetch-Guarded 方法

- single-assignment IVF layout。
- RaBitQ bound-guided triage。
- SafeIn/SafeOut/Uncertain 读取策略。
- dynamic io_uring fetch scheduling。

### 第 5 章 实现

- `ClusterProber`、`AsyncIOSink`、`OverlapScheduler`。
- 关键参数：`nprobe`、`safein_all_threshold`、`submit_batch_size`、`io_queue_depth`、`refill_threshold/refill_count`。
- 数据和 baseline 复用路径。

### 第 6 章 实验设置

- 数据集：COCO100K、MS MARCO、Deep1M_synth optional。
- Workload 差异：COCO 图文跨模态、MS MARCO 文本检索、Deep1M_synth payload 控制变量。
- Baselines：四组合。
- Metrics：`topk=10` 主指标，`topk=20` 补充，`topk=50` stress；quality、avg/p95/p99 E2E、bytes read、breakdown。
- matched-quality 规则。

### 第 7 章 实验结果

- 主实验：COCO + MS MARCO。
- baseline 对比：四组合。
- 机制 ablation：triage boundary、rerank pruning、payload prefetch、dynamic scheduling。
- top-k sensitivity：top10/top20/top50。
- payload sensitivity：Deep1M_synth / fallback。

### 第 8 章 讨论与局限

- 本文不启用 CRC early stop / RAIR / duplicate clustering 的原因与影响。
- MS MARCO qrels 或 embedding 准备风险。
- Lance runner 复用风险。
- Deep1M_synth 作为第三数据集的时间风险。

### 第 9 章 结论

- 总结 bound-guided selective fetch 与动态调度的 E2E 收益。

## Remaining Risks

- `msmarco_passage` 需要确认 embedding、query 与 qrels/ground truth 完整性。
- Lance 四组合是否已完全可复用仍需执行验证。
- 当前代码 SafeIn 使用静态 `d_k_`，SafeOut 使用动态 `dynamic_d_k`；论文必须按实现写。
- 若要严格改成 SafeIn/SafeOut 都围绕当前动态 top-k 半径，需要额外代码修改和 correctness 验证，不建议作为本科论文主线。
