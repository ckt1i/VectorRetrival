# Round 1 Refinement

## Problem Anchor

- Bottom-line problem: 在本科毕业论文时间约束下，将当前 VectorRetrival/BoundFetch 代码收敛为一版最小但完整、可复现、可写作的实验闭环，证明集成式向量检索与 payload 读取协同设计在真实检索和合成 payload 场景中优于拆分式 baseline。
- Must-solve bottleneck: 旧方向同时展开 CRC 早停、RAIR/重复聚类、CPU 热路径优化和大 baseline 矩阵，实验面过宽；当前必须缩小到单聚类划分、无 CRC 早停、有限数据集、有限 baseline 的主实验与 ablation，使论文能按期完成。
- Non-goals: 不证明 ConANN CRC 早停；不做 RAIR 或重复聚类；不做完整 `search x storage x dataset` 笛卡尔矩阵；不追求击败所有图索引或商业向量数据库；不新增复杂学习模块。
- Constraints: 数据集只考虑 `coco_100k`、`deep1m_synth`、`msmarco_passage`；其中 `deep1m_synth` 仍需生成，当前 `/home/zcq/VDB/data/formal_baselines` 下看到的是 `deep8m_synth`；baseline 只考虑 `IVF+PQ/IVF+RaBitQ` 与 `FlatStor/Lance` 的四种组合；当前代码执行时固定 `--crc 0`、`--assignment-mode single`，不启用 RAIR/重复聚类；复用 `/home/zcq/VDB/baselines` 现有脚本和结果格式；论文写作参考 VLDB/PVLDB 的系统论文结构。
- Success condition: 产出一套可执行实验矩阵、主实验图表、关键 ablation、方法叙事、创新点和毕业论文大纲；即使最终只完成 COCO 100K 与一个扩展数据集，也能形成自洽论文。

## Anchor Check

- Original bottleneck: 旧方向实验面过宽，缺少本科论文可按期完成的最小闭环。
- Why the revised method still addresses it: 修订后把主线冻结为 `COCO100K + FlatStor + IVF+PQ/IVF+RaBitQ + BoundFetch-Single`，`deep1m_synth` 只作为 payload 控制变量，`Lance/MS MARCO` 只作为附录备选。
- Reviewer suggestions rejected as drift: 未接受“增加更多现代组件”这一类潜在方向；当前问题是系统 serving 路径，不是 encoder 或 learned policy。

## Simplicity Check

- Dominant contribution after revision: single-assignment IVF 下的 integrated retrieve-fetch-rerank query plan。
- Components removed or merged: `msmarco_passage` 从主线降级为 appendix；`Lance` 从主 baseline 降级为 backup/appendix；ablation 只保留 overlap on/off 与 payload size sensitivity。
- Reviewer suggestions rejected as unnecessary complexity: 不再保留多 submission mode、full storage backend sweep、五点以上大 nprobe sweep。
- Why the remaining mechanism is still the smallest adequate route: 它只依赖现有代码可执行路径，不新增训练模块；实验足以证明“集成执行路径”相对拆分式 pipeline 的端到端收益。

## Changes Made

### 1. 方法从组件列表改成固定 query protocol

- Reviewer said: 需要明确候选预算、何时读取、如何公平对比 split baseline。
- Action: 新增 `top-C retrieve-fetch-rerank protocol`，固定每 query 候选预算 `C`，BoundFetch 和 split baseline 都按 matched recall 或 matched `C` 比较。
- Reasoning: 这样论文主张从“已有组件组合”变成可判定的系统策略。
- Impact on core method: 强化方法章节可实现性和实验公平性。

### 2. 实验矩阵再压缩

- Reviewer said: 3 数据集、4 组合、多 nprobe、多 ablation 仍过宽。
- Action: 主线冻结为 COCO100K + FlatStor + 两个搜索核；deep1m_synth 是扩展；Lance/MS MARCO 是 appendix/backlog。
- Reasoning: 本科论文成功条件是最小完整闭环，而不是缩小版 formal study。
- Impact on core method: 提高可完成性，降低实验失败风险。

### 3. 贡献表述收敛

- Reviewer said: benchmark protocol 不应成为独立主贡献。
- Action: 贡献压成一个主贡献和一个支撑贡献。
- Reasoning: 让论文读者记住一个核心机制。
- Impact on core method: 论文叙事更接近 VLDB/PVLDB 系统论文的中心故事。

## Revised Proposal

# Research Proposal: BoundFetch-Single 本科论文最小完整实验方案

## Problem Anchor

- Bottom-line problem: 在本科毕业论文时间约束下，将当前 VectorRetrival/BoundFetch 代码收敛为一版最小但完整、可复现、可写作的实验闭环，证明集成式向量检索与 payload 读取协同设计在真实检索和合成 payload 场景中优于拆分式 baseline。
- Must-solve bottleneck: 旧方向同时展开 CRC 早停、RAIR/重复聚类、CPU 热路径优化和大 baseline 矩阵，实验面过宽；当前必须缩小到单聚类划分、无 CRC 早停、有限数据集、有限 baseline 的主实验与 ablation，使论文能按期完成。
- Non-goals: 不证明 ConANN CRC 早停；不做 RAIR 或重复聚类；不做完整 `search x storage x dataset` 笛卡尔矩阵；不追求击败所有图索引或商业向量数据库；不新增复杂学习模块。
- Constraints: 数据集只考虑 `coco_100k`、`deep1m_synth`、`msmarco_passage`；其中 `deep1m_synth` 仍需生成，当前 `/home/zcq/VDB/data/formal_baselines` 下看到的是 `deep8m_synth`；baseline 只考虑 `IVF+PQ/IVF+RaBitQ` 与 `FlatStor/Lance` 的四种组合；当前代码执行时固定 `--crc 0`、`--assignment-mode single`，不启用 RAIR/重复聚类；复用 `/home/zcq/VDB/baselines` 现有脚本和结果格式；论文写作参考 VLDB/PVLDB 的系统论文结构。
- Success condition: 产出一套可执行实验矩阵、主实验图表、关键 ablation、方法叙事、创新点和毕业论文大纲；即使最终只完成 COCO 100K 与一个扩展数据集，也能形成自洽论文。

## Technical Gap

现代 embedding retrieval serving 的关键路径通常是 `retrieve -> fetch -> rerank`：先用 ANN 找候选，再读取原始向量、文本、图片 metadata 或 payload，最后做 rerank 或返回对象。拆分式 baseline 把向量检索和 payload 读取当成两个独立阶段，容易产生两个问题：

1. ANN 阶段只优化 vector-search latency，不知道后续 payload fetch 的访问形态。
2. payload 阶段只能被动读取 ANN 输出 id，难以与候选扫描和 rerank 重叠。

本文不试图证明新的量化算法，也不做 CRC/RAIR。核心缺口是：在 single-assignment IVF 这样足够简单的边界内，是否可以通过 integrated query plan 把候选筛选、payload/原向量读取和 rerank 合成一个服务路径，从而降低 matched-recall 下的 E2E latency。

## Method Thesis

- One-sentence thesis: BoundFetch-Single 使用 single-assignment IVF 和固定 top-`C` 候选预算，将 approximate retrieve、candidate fetch 和 exact rerank 合并为一个 query plan，在 matched-recall 条件下降低 payload-rich retrieval 的 E2E latency。
- Why this is the smallest adequate intervention: 它只要求冻结执行协议和实验口径，复用现有 `bench_e2e`、RaBitQ、cluster store 和 overlap scheduler，不新增 CRC、RAIR、重复聚类或训练组件。
- Why this route is timely: 当前 dense retrieval 系统的用户可感知成本来自完整 serving chain，而不是单独 ANN kernel；系统论文需要评估 top-k 对象返回的端到端路径。

## Contribution Focus

- Dominant contribution: `BoundFetch-Single`，一个 single-assignment IVF 下的 integrated retrieve-fetch-rerank query plan，定义固定候选预算、读取时机和 rerank 边界。
- Supporting contribution: 一个 matched-recall 的最小复现实验 protocol，用 COCO100K 主实验和 deep1m_synth payload sensitivity 验证系统收益来源。
- Explicit non-contributions: CRC early stop、RAIR、duplicate clustering、encoder 训练、Lance 主线优化、MS MARCO 完整 benchmark、图索引全量对比。

## Proposed Method

### Complexity Budget

- Frozen / reused backbone: `bench_e2e`、Faiss kmeans centroids、single cluster assignment、RaBitQ approximate scoring、current cluster/address layout、FlatStor baseline scripts。
- New trainable components: 无。
- Optional-only components: Lance 和 MS MARCO 只作为 appendix/backlog，不进入主线成功条件。
- Tempting additions intentionally not used: CRC、RAIR、multiple assignment、learned probe policy、extra reranker、Parquet、DiskANN、full storage sweep。

### BoundFetch-Single Query Protocol

每个 query 固定以下协议：

1. Coarse scoring: 对所有 centroid 打分，选择 `nprobe` 个 cluster。
2. Approximate retrieve: 在 probed clusters 内用 RaBitQ 近似距离评分。
3. Candidate budget: 选取 top-`C` 候选，`C` 是实验固定参数，例如 `C in {64,128,256}`；主表用 matched recall operating point，而不是任意挑最快点。
4. Fetch scheduling: top-`C` 候选一旦确定，就提交原向量/payload 读取；在实现允许时与后续 cluster 扫描、候选整理或 rerank 准备重叠。
5. Exact rerank: 只在 top-`C` 候选上做 exact rerank。
6. Return: 返回 top-`k`，默认 `k=10`。

公平对比规则：

- Split baseline 也必须使用相同 top-k 目标，并报告达到相同 recall target 时的 E2E latency。
- 若 split baseline 产生 candidate ids，则 payload fetch 使用相同 `C` 或最近 matched-recall operating point。
- 主表不比较单独 ANN latency，而比较 `retrieve + fetch + rerank` E2E latency。

### System Overview

```text
offline:
  embeddings + payload records
    -> Faiss kmeans centroids
    -> single cluster assignment
    -> per-cluster RaBitQ code + address layout
    -> FlatStor payload export

online BoundFetch-Single:
  query
    -> top-nprobe clusters
    -> RaBitQ scores inside clusters
    -> top-C candidate ids
    -> async original-vector / payload fetch
    -> exact rerank on C
    -> top-k payload-bearing results

split baseline:
  query
    -> IVF+PQ or IVF+RaBitQ candidate ids
    -> FlatStor payload fetch
    -> rerank / join
    -> top-k payload-bearing results
```

### Integration Parameters

主实验固定：

- `--crc 0`
- `--assignment-mode single`
- `--topk 10`
- `--queries 1000`，如 MS MARCO 后续进入 appendix 可按可用 query 裁剪
- `--clu-read-mode full_preload` 作为 warm-serving 主口径
- `--submission-mode shared`
- `nprobe={64,128,256}`，只在 COCO 主图需要时补 `512`
- `C={64,128,256}` 或映射到现有 candidate budget 参数；如果当前 binary 没有显式 `C` 参数，则用可控的 top candidate/rerank budget 字段替代，并在执行记录中说明。

### Modern Primitive Usage

不引入 LLM/RL 等新组件。本文的现代性来自 serving chain framing：CLIP/BGE/E5 等 encoder 生成 embedding 后，真实系统还必须取回 payload 并完成对象级返回。本文优化的是该 serving chain，而不是训练 encoder。

## Minimal Experiment Plan

### Mandatory: COCO100K 主实验

目的：证明 integrated retrieve-fetch-rerank query plan 在真实图文 payload 场景下优于拆分式 pipeline。

数据与系统：

- Dataset: `coco_100k`
- Method: `BoundFetch-Single`
- Baselines: `IVF+PQ+FlatStor`、`IVF+RaBitQ+FlatStor`
- Optional appendix only: `IVF+PQ+Lance`、`IVF+RaBitQ+Lance`

参数：

- `topk=10`
- `queries=1000`
- `nlist=2048`
- `nprobe={64,128,256}`
- operating point: 主表选择 matched `recall@10`，例如最接近 `0.95` 或最高共同 recall 区间。

图表：

- 主表：matched recall 下 `avg/p95/p99 E2E latency`
- 主图：COCO recall-latency curve
- 分解图：ANN / fetch / wait / rerank latency

### Required Ablation: overlap on/off

目的：证明收益来自 integrated execution，而不是仅来自 RaBitQ 或更快 ANN。

设置：

- Dataset: `coco_100k`
- Backend: `FlatStor`
- Method variants: overlap on vs overlap off / serial fetch
- Baseline: `IVF+RaBitQ+FlatStor`

指标：

- `avg/p95/p99 E2E latency`
- latency breakdown
- overlap ratio 或 wait time reduction

### Second: deep1m_synth payload sensitivity

目的：隔离 payload 大小对系统收益的影响。

设置：

- Dataset: `deep1m_synth`
- Payload sizes: mandatory `4KB`；如果时间允许补 `256B` 和 `16KB` 或 `64KB`
- Systems: `BoundFetch-Single`、`IVF+PQ+FlatStor`、`IVF+RaBitQ+FlatStor`
- `nprobe` 只选 1-2 个 matched-recall operating points。

图表：

- payload size vs E2E latency
- payload size vs speedup

### Appendix / Backup

- `Lance`: 只有在 runner 已经可复用且不影响主线时加入。
- `msmarco_passage`: 只有在 embedding、query、qrels/ground truth 已准备时作为文本 workload sanity check。

## Claim-Driven Validation

### Claim 1: Integrated query plan reduces E2E serving latency at matched recall

- Minimal experiment: COCO100K 上比较 BoundFetch-Single、`IVF+PQ+FlatStor`、`IVF+RaBitQ+FlatStor`。
- Metric: matched `recall@10` 下 `avg/p95/p99 E2E latency`。
- Expected evidence: BoundFetch-Single 在同等 recall 区间 E2E latency 更低。

### Claim 2: The gain comes from fetch/rerank integration, not only from the approximate scorer

- Minimal experiment: overlap on/off 和 latency breakdown。
- Metric: fetch wait、rerank time、E2E tail latency。
- Expected evidence: 关闭 overlap 或串行化 fetch 会显著增加 wait/E2E latency。

### Claim 3: The gain grows or remains robust when payload cost increases

- Minimal experiment: deep1m_synth payload size sensitivity。
- Metric: payload size vs E2E latency/speedup。
- Expected evidence: payload 变大时，拆分式 pipeline 更受串行 fetch 影响，BoundFetch-Single 至少保持方向一致。

## Thesis Method and Innovation Points

### Method Name

使用 `BoundFetch-Single`。这个名字明确承认本文版本只研究 single-assignment，不承诺 CRC、RAIR 或 duplicate clustering。

### Paper-Writable Method Statement

本文提出 BoundFetch-Single，一个面向 payload-rich embedding retrieval 的集成执行路径。它在 single-assignment IVF 中使用固定 top-`C` 候选预算，把近似候选生成、payload/原向量读取和 exact rerank 放入同一个 query plan，并在 matched-recall 下评估完整 `retrieve -> fetch -> rerank` serving latency。

### Innovation Points

1. Integrated retrieve-fetch-rerank query plan: 以 top-`C` 候选预算定义读取和 rerank 边界，避免 ANN 与 payload fetch 完全分离。
2. Minimal single-assignment design: 保留 IVF 构建和解释简单性，证明不依赖 CRC/RAIR 也能形成系统收益。
3. Matched-recall E2E evaluation: 以对象返回服务链路为评估对象，报告 avg 与 tail latency，而不是只报告 ANN kernel 时间。

## VLDB/PVLDB-Style Thesis Outline

### 第 1 章 引言

- dense retrieval serving 已从 ANN 查询扩展为 `retrieve -> fetch -> rerank`。
- 拆分式 pipeline 的 latency 与 tail 问题。
- 本文贡献：BoundFetch-Single 和最小复现实验。

### 第 2 章 背景与相关工作

- IVF、PQ、RaBitQ。
- 向量数据库与 payload retrieval。
- 系统论文中的 matched-recall 和 tail-latency 评估。

### 第 3 章 问题定义与动机

- 定义 E2E latency、matched recall、payload fetch。
- 说明 non-goals：CRC、RAIR、duplicate clustering。
- 动机实验：拆分式 `ANN -> fetch` 的串行等待。

### 第 4 章 BoundFetch-Single

- single-assignment IVF layout。
- top-`C` retrieve-fetch-rerank protocol。
- fetch scheduling 与 exact rerank。
- 公平对比规则。

### 第 5 章 实现

- `bench_e2e` 参数。
- FlatStor payload layout。
- baseline 脚本复用。
- warm-serving 与缓存协议。

### 第 6 章 实验设置

- Mandatory: COCO100K。
- Second: deep1m_synth。
- Appendix: Lance/MS MARCO。
- 指标、参数、硬件环境。

### 第 7 章 实验结果

- 主表：matched recall E2E latency。
- 主图：COCO recall-latency。
- 因果 ablation：overlap on/off。
- 扩展图：payload size sensitivity。

### 第 8 章 讨论与局限

- 为什么裁剪 CRC/RAIR。
- `deep1m_synth` 与真实 workload 的差距。
- Lance/MS MARCO 后续扩展。

### 第 9 章 结论

- 总结 integrated serving path 的收益。
- 总结本科论文范围内可复现结论。

## Compute & Timeline Estimate

- Days 1-3: COCO100K 主闭环，生成主表和主图。
- Days 4-6: overlap on/off ablation，完成 latency breakdown。
- Days 7-10: deep1m_synth 生成与 payload size sensitivity。
- Days 11-14: 图表冻结、论文实验章节和方法章节成稿。
- GPU-hours: 若 embedding 已有，核心为 CPU/I/O 实验；若补 MS MARCO appendix，另计 embedding 生成成本。
