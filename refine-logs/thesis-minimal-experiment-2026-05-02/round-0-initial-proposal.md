# Research Proposal: BoundFetch-Lite 本科论文最小完整实验方案

## Problem Anchor

- Bottom-line problem: 在本科毕业论文时间约束下，将当前 VectorRetrival/BoundFetch 代码收敛为一版最小但完整、可复现、可写作的实验闭环，证明集成式向量检索与 payload 读取协同设计在真实检索和合成 payload 场景中优于拆分式 baseline。
- Must-solve bottleneck: 旧方向同时展开 CRC 早停、RAIR/重复聚类、CPU 热路径优化和大 baseline 矩阵，实验面过宽；当前必须缩小到单聚类划分、无 CRC 早停、有限数据集、有限 baseline 的主实验与 ablation，使论文能按期完成。
- Non-goals: 不证明 ConANN CRC 早停；不做 RAIR 或重复聚类；不做完整 `search x storage x dataset` 笛卡尔矩阵；不追求击败所有图索引或商业向量数据库；不新增复杂学习模块。
- Constraints: 数据集只考虑 `coco_100k`、`deep1m_synth`、`msmarco_passage`；其中 `deep1m_synth` 仍需生成，当前 `/home/zcq/VDB/data/formal_baselines` 下看到的是 `deep8m_synth`；baseline 只考虑 `IVF+PQ/IVF+RaBitQ` 与 `FlatStor/Lance` 的四种组合；当前代码执行时固定 `--crc 0`、`--assignment-mode single`，不启用 RAIR/重复聚类；复用 `/home/zcq/VDB/baselines` 现有脚本和结果格式；论文写作参考 VLDB/PVLDB 的系统论文结构。
- Success condition: 产出一套可执行实验矩阵、主实验图表、关键 ablation、方法叙事、创新点和毕业论文大纲；即使最终只完成 COCO 100K 与一个扩展数据集，也能形成自洽论文。

## Technical Gap

当前已有两条日志分别面向 BoundFetch 优化和 formal baseline 扩展。它们的问题不是方向错误，而是对本科论文而言过宽：一条继续追 query-prep/probe 热路径，另一条规划了更多数据集、更多 baseline 与更多后端。新的论文目标需要把系统贡献裁剪成一个可验证的核心：在 IVF 粗聚类和单聚类 assignment 下，把候选筛选、payload 定位、异步读取和 rerank 作为一个端到端执行路径来评估，而不是把 ANN latency 与 payload latency 事后相加。

最小技术缺口是：拆分式 baseline 先跑向量检索，再按 top-k 或候选 id 访问 FlatStor/Lance；它缺少对候选生成、payload 预取、I/O overlap 和 rerank 顺序的一体化控制。当前代码已经有 `bench_e2e`、`overlap_scheduler`、RaBitQ estimator、FlatStor-like cluster store 与 result collector，可以支撑这个故事。无需 CRC 早停或重复聚类也能形成一个清晰方法：BoundFetch-Lite。

## Method Thesis

- One-sentence thesis: BoundFetch-Lite 在单聚类 IVF 框架内，将 RaBitQ 候选筛选、cluster-resident 元数据、异步 payload/向量读取和 exact rerank 合并为一个端到端执行计划，从而减少拆分式 ANN+payload pipeline 的额外访存与串行等待。
- Why this is the smallest adequate intervention: 它保留现有核心代码路径，只冻结 CRC 与重复聚类，把论文主张落到“集成执行计划”而不是额外算法模块。
- Why this route is timely in the foundation-model era: 现代 embedding 检索已从纯 ANN 变成“向量召回 + 原始对象/文本读取 + rerank/展示”的服务链路；方法关注的是 embedding 检索系统的端到端 serving bottleneck，而不是重新训练 encoder。

## Contribution Focus

- Dominant contribution: 一个面向 payload-rich embedding retrieval 的轻量级集成检索执行路径，统一候选筛选、I/O 调度和 rerank，并用真实多模态/文本数据与合成 payload 数据验证。
- Optional supporting contribution: 一套最小但可复现的 benchmark protocol，将 `IVF+PQ`、`IVF+RaBitQ` 与 `FlatStor/Lance` 四种拆分式组合放在相同数据、top-k、nprobe 和 warm-serving 口径下比较。
- Explicit non-contributions: CRC conformal early stop、RAIR、多重 assignment、encoder 训练、完整向量数据库产品对比、DiskANN/ScaNN/HNSW 大规模调参。

## Proposed Method

### Complexity Budget

- Frozen / reused backbone: 现有 `bench_e2e`、Faiss coarse kmeans、single assignment IVF layout、RaBitQ encoder/estimator、overlap scheduler、FlatStor/Lance baseline 脚本。
- New trainable components: 无。
- Tempting additions intentionally not used: CRC early stop、RAIR、duplicate clustering、learned cluster policy、额外 reranker、更多 storage backend、更多 dataset。

### System Overview

```text
offline:
  base embeddings + payload records
    -> Faiss kmeans centroids
    -> single cluster assignment
    -> per-cluster compact vector/code/address layout
    -> FlatStor/Lance baseline exports

online BoundFetch-Lite:
  query embedding
    -> coarse centroid scoring
    -> probe top-nprobe clusters
    -> RaBitQ approximate candidate scoring
    -> schedule original-vector/payload reads with overlap
    -> exact rerank
    -> top-k ids + payload

split baseline:
  query embedding
    -> IVF+PQ or IVF+RaBitQ vector search
    -> top-k/candidate ids
    -> FlatStor or Lance payload fetch
    -> joined E2E latency
```

### Core Mechanism

- Input / output: 输入为 query embedding、base embedding index、payload store；输出为 top-k id、score 与 payload。
- Architecture or policy: 使用单聚类 IVF 作为访问边界；每个 query 先选择 `nprobe` 个 cluster；在 cluster 内用 RaBitQ 近似距离得到候选；调度器根据候选读取需求提交异步读取，并与候选扫描/rerank 重叠。
- Training signal / loss: 无训练；核心是系统执行路径与可复现实验 protocol。
- Why this is the main novelty: 对比对象不是单独 ANN 算法，而是拆分式 `vector search -> payload fetch` pipeline。BoundFetch-Lite 的新意在于把候选生成和对象读取合并到一个计划中，用 cluster layout 与调度策略降低端到端 latency。

### Modern Primitive Usage

- Which LLM / VLM / Diffusion / RL-era primitive is used: 不引入额外 foundation model 组件。
- Exact role in the pipeline: 使用 CLIP/SigLIP 或 BGE/E5 类 embedding 作为输入数据来源，但不把 encoder 训练作为贡献。
- Why it is more natural than an old-school alternative: 当前瓶颈是 embedding retrieval serving 的系统路径；强行加入 LLM/RL 会增加复杂度且偏离问题锚点。

### Integration into Downstream Pipeline

执行时固定：

- `--crc 0`
- `--assignment-mode single`
- `--clu-read-mode full_preload` 作为主 warm-serving 口径
- `--submission-mode shared`
- `--use-resident-clusters 0` 先保持和现有 precheck 脚本一致，后续只作为 ablation
- 不启用 RAIR 或重复聚类

baseline 复用：

- `/home/zcq/VDB/baselines/scripts/coco100k_top10_precheck_run_ivf_pq_flatstor.sh`
- `/home/zcq/VDB/baselines/scripts/coco100k_top10_precheck_run_ivf_rabitq_flatstor.sh`
- `/home/zcq/VDB/baselines/payload_retrieval/build_datasets.py`
- `/home/zcq/VDB/baselines/e2e/run_e2e_comparison.py`

Lance 组合优先复用 `/home/zcq/VDB/baselines/payload_retrieval` 与 formal-study workspace 中的 export/runner 结构。

## Claim-Driven Validation Sketch

### Claim 1: 集成执行路径优于拆分式 pipeline

- Minimal experiment: 在 `coco_100k` 上比较 BoundFetch-Lite 与四种 baseline：`IVF+PQ+FlatStor`、`IVF+PQ+Lance`、`IVF+RaBitQ+FlatStor`、`IVF+RaBitQ+Lance`。
- Baselines / ablations: 固定 `topk=10`，`nlist=2048`，扫 `nprobe={32,64,128,256,512}` 或按可完成性裁剪到 `{64,128,256}`。
- Metric: `recall@10`、avg/p50/p95/p99 E2E latency、ANN core latency、payload fetch latency、bytes read、build time、index/store size。
- Expected evidence: 在相同 recall 区间，BoundFetch-Lite 的 E2E latency 低于拆分式组合，尤其在 payload fetch 占比变高时更明显。

### Claim 2: 收益来自系统协同，而不是单纯量化器选择

- Minimal experiment: 对 BoundFetch-Lite 做 ablation：关闭 overlap / 改为串行 fetch、改变 `nprobe`、改变 candidate budget、PQ-like baseline 与 RaBitQ baseline 对齐候选预算。
- Baselines / ablations: `overlap on/off`、`shared vs isolated submission`、`full_preload vs on-demand`、`RaBitQ vs PQ baseline`。
- Metric: E2E latency decomposition、overlap ratio、recall-latency curve。
- Expected evidence: 关闭 overlap 或把 payload fetch 后移为独立阶段会损失 latency，即使 ANN recall 接近。

### Claim 3: 方法在不同数据和 payload 形态下保持方向一致

- Minimal experiment: 扩展到 `msmarco_passage` 与 `deep1m_synth`；其中 `deep1m_synth` 生成 `256B/4KB/16KB` 或 `256B/4KB/64KB` payload 档位。
- Baselines / ablations: 只保留最关键的四种 baseline 组合；若时间不足，先跑 `FlatStor` 两种搜索核，再补 Lance。
- Metric: recall@10/MRR@10/nDCG@10、E2E latency、payload size sensitivity。
- Expected evidence: 文本真实数据和合成 payload 控制变量下，集成路径的优势随 payload 成本增大而增强或至少不退化。

## Experiment Handoff Inputs

- Must-prove claims: BoundFetch-Lite 在 warm-serving E2E 下优于拆分式 IVF+PQ/RaBitQ + FlatStor/Lance；收益来自集成调度与读取重叠；不依赖 CRC 或重复聚类。
- Must-run ablations: overlap on/off；`nprobe`/candidate budget；FlatStor vs Lance；synthetic payload size；single assignment 固定性检查。
- Critical datasets / metrics: `coco_100k` 为主实验；`msmarco_passage` 为真实文本扩展；`deep1m_synth` 为 payload 控制变量；指标为 `recall@10`、E2E latency、tail latency、latency breakdown、index/store size。
- Highest-risk assumptions: Lance runner 是否完全可复用；`deep1m_synth` 是否能及时生成；MS MARCO embedding/ground truth 是否已准备；不同 baseline 的 warm cache 口径是否一致。

## 快速实验方案

### Phase A: 三天内完成 COCO 100K 主闭环

1. 确认 `bench_e2e` release binary、COCO 100K embedding、centroids、assignments、ground truth 均可用。
2. 固定 `--crc 0 --assignment-mode single --topk 10 --queries 1000`。
3. 跑 BoundFetch-Lite `nprobe={64,128,256,512}`。
4. 跑 `IVF+PQ+FlatStor` 和 `IVF+RaBitQ+FlatStor`。
5. 若 Lance 已可执行，补 `IVF+PQ+Lance` 和 `IVF+RaBitQ+Lance`；否则先写入风险表，不阻塞主线。
6. 生成第一版 recall-latency curve 与 latency breakdown。

### Phase B: 一周内完成扩展与 ablation

1. 生成或确认 `deep1m_synth`，至少完成一个 payload size；理想完成 3 个 size。
2. 准备 `msmarco_passage` embedding、query、qrels 或 ANN ground truth。
3. 在两个扩展数据集上跑最小矩阵：BoundFetch-Lite、IVF+PQ+FlatStor、IVF+RaBitQ+FlatStor。
4. 完成 overlap on/off、submission mode、payload size sensitivity。
5. 统一输出 CSV schema，保证论文表格可直接生成。

### Phase C: 最后一周冻结论文图表

1. 主表：三数据集上最接近 `recall@10 >= 0.95` 的 E2E latency。
2. 主图：COCO 100K recall-latency curve。
3. 分解图：ANN / payload / wait / rerank latency breakdown。
4. ablation 图：overlap on/off、payload size、nprobe sensitivity。
5. 资源表：build time、index bytes、payload store bytes。

## 可写进毕业论文的方法与创新点

### 方法命名

建议论文中使用 `BoundFetch-Lite` 或 `BoundFetch-Single`，避免继续承诺 CRC/RAIR 相关机制。

### 方法章节主张

BoundFetch-Lite 不是新的 encoder，也不是单独 ANN 算法，而是一个面向 embedding retrieval serving 的系统执行框架。它把 IVF cluster、RaBitQ 近似评分、payload 地址布局、异步读取和 exact rerank 合并成同一个 query plan，使系统能在生成候选时提前安排后续读取，并避免拆分式 baseline 的串行等待。

### 创新点

1. 端到端检索与 payload 获取的一体化执行路径：把 ANN candidate generation 与对象读取合并评估，而不是事后相加。
2. 面向 cluster 的轻量数据布局：单聚类 assignment 保持构建简单，同时让 per-cluster code、address、payload 访问更容易被预取和调度。
3. 与量化器解耦的系统收益验证：通过 `IVF+PQ` 和 `IVF+RaBitQ` 对照说明收益不是来自某一个弱 baseline。
4. 最小可复现 benchmark protocol：三类数据覆盖图文、文本和合成 payload 控制变量，四种 baseline 组合覆盖搜索核与 storage backend。

## 参考 VLDB/PVLDB 的毕业论文大纲

### 第 1 章 引言

- embedding retrieval 从纯向量检索转向向量召回、payload 读取和 rerank 的服务链路。
- 拆分式 ANN + payload fetch pipeline 的局限。
- 本文目标、约束与贡献。

### 第 2 章 背景与相关工作

- IVF、PQ、RaBitQ。
- payload-aware retrieval / vector database serving。
- FlatStor、Lance 与列式/对象式 payload 存储。
- 与 DiskANN/图索引的边界：作为相关系统，不作为本轮主 baseline。

### 第 3 章 问题定义与动机实验

- 定义 E2E latency、recall、payload fetch、warm-serving 口径。
- 展示拆分式 pipeline 的 latency decomposition。
- 明确本文不研究 CRC early stop、RAIR、多重 assignment。

### 第 4 章 BoundFetch-Lite 方法

- 数据布局与单聚类 assignment。
- query-time pipeline。
- RaBitQ 候选评分与 exact rerank。
- overlap scheduler 与 payload/向量读取。
- 与 `IVF+PQ/RaBitQ + FlatStor/Lance` 拆分式 baseline 的区别。

### 第 5 章 实现

- `bench_e2e` 参数与执行模式。
- 数据格式、cluster store、payload store。
- baseline 复用路径。
- reproducibility：硬件、编译、参数、缓存协议、输出 CSV。

### 第 6 章 实验设置

- 数据集：COCO 100K、MS MARCO Passage、Deep1M synthetic payload。
- baseline：四种组合。
- 指标：recall@k、MRR/nDCG、avg/p95/p99 latency、index/store size、build time。
- 参数 sweep 与固定项。

### 第 7 章 实验结果

- 主实验：三数据集 E2E 对比。
- recall-latency curve。
- latency breakdown。
- ablation：overlap、storage backend、payload size、nprobe/candidate budget。

### 第 8 章 讨论与局限

- 为什么不包含 CRC/RAIR。
- Lance 可复用性和生产系统差距。
- synthetic payload 的外部有效性。
- 扩展到更大数据集和图索引的可能性。

### 第 9 章 结论

- 总结 BoundFetch-Lite 的端到端收益。
- 总结最小实验闭环与未来工作。

## Compute & Timeline Estimate

- Estimated GPU-hours: 0-8 GPU 小时，主要取决于 MS MARCO embedding 是否已存在；如果 embedding 已有，则核心实验主要消耗 CPU/内存/磁盘 I/O。
- CPU / storage: COCO 100K 可在单机快速迭代；MS MARCO 与 Deep1M synthetic 需要预留数百 GB 级别临时空间，取决于 payload size。
- Timeline: COCO 主闭环 2-3 天；扩展数据与 ablation 4-7 天；图表冻结和论文写作 7-10 天。
