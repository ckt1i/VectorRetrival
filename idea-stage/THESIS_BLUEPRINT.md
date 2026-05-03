# Thesis Blueprint: BoundFetch-Guarded

**Direction**: 基于 `thesis-minimal-experiment-2026-05-02` 的 refine 记录、实验方案和当前代码实现，形成本科毕业论文写作框架。  
**Date**: 2026-05-03  
**Revision Focus**: 本版只保留当前本科论文主线：FlatStor 后端上的 integrated vector retrieval + payload fetch；Lance 作为 benchmark/reference backend；COCO100K 与 MS MARCO 作为主实验数据集；更大 top-k 作为补充观察。

## Executive Summary

论文主线应写成一个系统型本科论文：

**BoundFetch-Guarded 面向 payload-rich vector retrieval，将向量检索、候选精排和 payload 读取放进同一个端到端执行管线。系统利用 RaBitQ 估计距离的误差界，将候选划分为 SafeIn、SafeOut 和 Uncertain，并为三类候选分配不同的读取策略，从而减少不必要的原向量读取、payload materialization 和尾部等待。**

当前论文框架不需要讨论所有未来投稿版本的实现边界，也不需要把所有暂缓实验写进方法章节。本科论文应优先让读者理解三件事：

1. 真实检索服务的瓶颈不是单纯 ANN 搜索，而是 `search -> rerank -> fetch payload` 的端到端路径。
2. 候选有置信度差异，BoundFetch-Guarded 用误差界把这种差异转化为 I/O 决策。
3. 在 COCO100K 和 MS MARCO 上，FlatStor 后端的 integrated pipeline 相比拆分式 baseline 有更好的 matched-quality latency。

## Paper Claims

### C1: End-to-End Latency Claim

在 COCO100K 与 MS MARCO 两个真实 workload 上，BoundFetch-Guarded 在 matched recall@10 条件下降低端到端查询延迟，尤其是 p95/p99 tail latency。

### C2: Bound-Guided Triage Claim

BoundFetch-Guarded 的主要收益来自候选三分类：

- SafeIn: 高置信候选，提前读取原向量和 payload，减少最终 materialization 等待。
- SafeOut: 低置信候选，直接跳过，减少读取和 rerank 放大。
- Uncertain: 边界候选，先读原向量做 exact rerank，只有最终进入 top-k 时再读取 payload。

### C3: I/O Scheduling Claim

动态异步读取调度把 cluster probe、vector read、payload read 和 rerank 重叠起来；系统收益不只是来自更快的距离估计，也来自更合理的 I/O 提交和完成处理。

### C4: Top-k Robustness Claim

当返回集合从 top-10 扩大到 top-20、top-50 或 top-100 时，候选读取和 payload materialization 的压力上升。补充实验应观察 BoundFetch-Guarded 是否仍能控制 read amplification 和 tail latency。

## Implementation Mapping

| Paper Concept | Current Implementation | Thesis Usage |
|---------------|------------------------|--------------|
| Coarse search | `OverlapScheduler::Search` calls IVF cluster selection | 方法背景，不作为创新点 |
| Candidate scoring | `ClusterProber::Probe` + RaBitQ estimator | 候选三分类的基础 |
| SafeIn / SafeOut / Uncertain | `ClusterProber` classification and `AsyncIOSink` read-plan generation | 方法章节核心 |
| Vector-only read | `VEC_ONLY` path, then exact rerank | Uncertain 策略 |
| Vector + payload read | `VEC_ALL` path | SafeIn 策略 |
| Missing payload fetch | final fetch after rerank | 端到端执行闭环 |
| Dynamic submit | `ProbeAndDrainInterleaved`, `EmitPendingDataRequests` | I/O 调度章节 |
| Benchmark output | `bench_e2e` metrics and JSON output | 实验统计来源 |

论文实现章节应讲清楚系统由哪些模块组成，以及它们如何服务于方法主线。不要把暂缓的未来扩展写成当前论文需要解释的边界。

## Detailed Thesis Outline

### 第 1 章 引言

**目标**: 从端到端 vector retrieval 服务问题引出本文方法。

建议小节：

1.1 背景：向量检索已经成为多模态检索、语义搜索和 RAG 系统的基础组件。  
1.2 问题：实际查询需要返回 records，而不是只返回 vector ids；payload 读取和精排会影响端到端延迟。  
1.3 观察：ANN 候选并不等价，高置信、低置信和边界候选应使用不同读取策略。  
1.4 方法概述：BoundFetch-Guarded 用误差界驱动候选三分类，并把分类结果映射到不同 I/O read plans。  
1.5 贡献：

- 提出 bound-guided selective fetch pipeline。
- 实现候选级 vector/payload 读取策略和动态异步调度。
- 在 COCO100K 与 MS MARCO 上以 matched-quality latency 评估，并用消融验证机制贡献。

建议图：

- Figure 1: Split ANN + payload fetch pipeline vs BoundFetch-Guarded integrated pipeline。

### 第 2 章 背景与相关工作

**目标**: 给读者补齐 ANN、量化、payload storage 和异步 I/O 背景。

建议小节：

2.1 Approximate Nearest Neighbor Search  
介绍 IVF、PQ、graph ANN、disk-aware ANN 的基本背景。本文实验聚焦 IVF-family，因为该设计空间便于控制量化器和 storage backend。

2.2 Quantization-Based Vector Search  
介绍 PQ、Faiss、RaBitQ。重点说明本文复用 RaBitQ 的误差界作为候选分类依据，而不是重新提出一个量化算法。

2.3 Payload-Aware Vector Storage  
介绍 FlatStor 与 Lance。本文主后端采用 FlatStor；Lance 作为 benchmark/reference backend，用于对照更通用的列式 multimodal/vector storage。

2.4 Asynchronous I/O for Storage Engines  
介绍 io_uring、batch submit、queue depth、polling、registered buffers、compute/I-O overlap 等系统背景。这里可以引用 `优化技巧.md` 中两篇 I/O 论文。

2.5 本文定位  
本文不是纯 ANN 算法论文，也不是通用文件格式论文；本文关注如何把候选级置信度转化为端到端 retrieval pipeline 中的 I/O 决策。

### 第 3 章 问题定义与动机

**目标**: 定义论文解决的问题和评价口径。

建议小节：

3.1 Query Model  
输入 query vector，输出 top-k records。每个 record 包括 id、distance 和 payload。

3.2 End-to-End Cost Model  
把一次查询拆成：

- coarse cluster selection
- compressed candidate scoring
- original vector read
- exact rerank
- payload read
- result materialization

3.3 Matched-Quality Evaluation  
给定质量阈值 `Q*`，比较每个系统达到 `Q*` 所需的最低 E2E latency。主质量指标使用 recall@10；top-20/top-50/top-100 作为补充扩展指标。

3.4 Motivation Study  
从已完成的 COCO 或 MS MARCO 主实验中选一两个 operating points，展示：

- 候选被划分为 SafeIn/SafeOut/Uncertain 后的比例
- baseline 中 rerank candidate 和 payload fetch 的放大
- BoundFetch-Guarded 如何减少不必要读取
- tail latency 中 I/O 等待的占比

建议图表：

- Figure 2: SafeIn/SafeOut/Uncertain 三分类示意图。
- Figure 3: candidate flow stacked bar: probed candidates -> triage -> rerank -> final payload fetch。

### 第 4 章 方法：BoundFetch-Guarded

**目标**: 给出方法设计，而不是陷入代码参数。

建议小节：

4.1 Overview  
展示完整 pipeline：cluster probe -> bound-guided triage -> read-plan generation -> async fetch -> exact rerank -> missing payload fetch。

4.2 Bound-Guided Candidate Triage  
定义 estimated distance、error margin、当前 top-k frontier 的直觉。候选分为：

- SafeIn: 估计上足够接近 query
- SafeOut: 估计上不可能进入 top-k
- Uncertain: 需要 exact rerank 判断

4.3 Selective Fetch Policy  
分别写三类候选的读取策略：

- SafeIn: vector + payload read
- SafeOut: skip
- Uncertain: vector-only read followed by rerank, then delayed payload fetch if selected

4.4 Dynamic Async Fetch Scheduling  
解释为什么不能等所有 cluster probe 完再统一读取。BoundFetch-Guarded 维护不同 read-plan queues，并根据 pending request、queue depth、batch size 和查询进度提交 I/O，使计算和读取重叠。

4.5 Correctness and Quality Control  
强调系统仍以 recall@k 为质量约束。SafeOut 需要保守阈值控制；Uncertain 走 exact rerank；最终输出通过 matched-quality 规则评估。

建议算法：

- Algorithm 1: BoundFetch-Guarded Query Processing
- Algorithm 2: Bound-Guided Read Plan Generation

### 第 5 章 系统实现

**目标**: 说明系统如何落地，但不要写成参数堆砌。

建议小节：

5.1 Data Layout on FlatStor  
描述主后端 FlatStor 中向量、payload、地址元数据和 cluster block 的组织方式。论文应把 FlatStor 写成本文主实验后端。

5.2 Query Execution Engine  
描述 `OverlapScheduler` 如何组织一次查询，包括 cluster selection、prefetch、candidate reads、rerank 和 final payload fetch。

5.3 Candidate Classification Engine  
描述 `ClusterProber` 的 batch scoring、候选分类和 Stage-2 reclassification。Stage-2 可以写成实现优化，不必作为论文主贡献单独拔高。

5.4 Read Plan and Buffer Management  
描述 `VEC_ONLY`、`VEC_ALL`、buffer pool、pending queues 和 final missing payload fetch。

5.5 Benchmark Harness and Metrics Export  
描述 `bench_e2e` 如何输出 recall、avg/p50/p95/p99 latency、triage counts、rerank count、payload fetch count、I/O submit stats。

建议表：

- Table 1: System components and responsibilities。
- Table 2: Main runtime metrics exported by benchmark。

### 第 6 章 实验设置

**目标**: 明确数据、baseline、参数和指标，避免实验解释漂移。

6.1 Research Questions:

- RQ1: BoundFetch-Guarded 是否在真实 workload 上降低 matched-quality E2E latency？
- RQ2: SafeIn/SafeOut/Uncertain 三分类分别贡献了什么？
- RQ3: 动态异步读取调度是否改善 tail latency？
- RQ4: 更大 top-k 下，BoundFetch-Guarded 是否仍能控制 read amplification？

6.2 Datasets:

- COCO100K: 图文/多模态检索 workload。
- MS MARCO Passage: 文本 dense retrieval workload。

6.3 Systems Compared:

主后端：

- BoundFetch-Guarded + FlatStor
- IVF+PQ + FlatStor
- IVF+RaBitQ + FlatStor

Benchmark/reference backend:

- IVF+PQ + Lance
- IVF+RaBitQ + Lance

6.4 Protocol:

- `queries=1000`
- COCO: `nlist=2048`, `nprobe=16,32,64,128,256,512`
- MS MARCO: `nlist=16384`, `nprobe=16,32,64,128,256,512,1024`
- top-k: top10 main, top20/top50/top100 supplement if output support is ready
- warm-serving protocol for main tables

6.5 Metrics:

- recall@10 as main quality metric
- recall@20 / recall@50 / recall@100 for larger top-k sensitivity
- avg/p50/p95/p99 E2E latency
- bytes read
- reranked candidates
- SafeIn/SafeOut/Uncertain ratio
- payload-prefetched reads and final missing payload fetches
- submit calls, flush counts, I/O wait time

6.6 Matched-Quality Rule:

- 对每个 dataset，选择所有主系统共同可达的质量阈值 `Q*`。
- 每个系统报告达到 `Q*` 的最低 latency。
- 曲线保留完整 nprobe sweep，主表只汇总 matched-quality operating point。

### 第 7 章 实验结果

**目标**: 每一节回答一个 RQ，避免只罗列表格。

7.1 Main Result on COCO100K  
展示 recall@10 vs E2E latency 曲线、matched-quality 表、p95/p99 对比和主要 breakdown。

7.2 Main Result on MS MARCO  
同样展示 recall@10 vs E2E latency。保持与 COCO 一致的 ANN recall 评价口径。

7.3 Backend Benchmark: FlatStor and Lance  
展示 FlatStor 主后端与 Lance benchmark 的对比。重点不是证明 Lance 优劣，而是说明 BoundFetch-Guarded 在主后端上的收益，并观察更通用 backend 下的 baseline 行为。

7.4 Bound-Guided Triage Ablation  
建议消融：

- Full BoundFetch-Guarded
- SafeOut pruning removed
- SafeIn payload prefetch removed
- Uncertain eager payload
- boundary sensitivity

主要看：

- reranked candidates
- bytes read
- payload-prefetched vs final payload fetch
- recall@10
- p95/p99 latency

7.5 Dynamic Scheduling Ablation  
建议消融：

- submit-online on/off
- submit-batch sweep
- queue-depth sensitivity
- safein payload threshold sensitivity

主要看：

- avg/p95/p99 latency
- I/O wait time
- submit calls
- flush counts

7.6 Top-k Sensitivity  
如果 benchmark 已支持更大 recall 输出，则观察 top20、top50、top100。主文建议至少放 top20；top50/top100 可放补充材料或附录。

### 第 8 章 讨论

**目标**: 诚实解释结果边界，而不是讨论尚未进入本科论文的未来扩展。

建议内容：

- 为什么 integrated retrieval 对 payload-rich workload 有意义。
- 哪些场景下 SafeIn 预取收益更大，哪些场景下 Uncertain delayed payload 更重要。
- 大 top-k 下，最终必须 materialize 的 payload 数量增加，selective fetch 的收益可能下降。
- FlatStor 与 Lance 的对比说明 storage backend 会影响 payload fetch 成本。
- 当前论文以 IVF-family 和 FlatStor 主后端为中心，结论应限制在这个设计空间内。

### 第 9 章 结论

总结：

- BoundFetch-Guarded 将 RaBitQ bound 转化为候选级 I/O 策略。
- Selective fetch 减少无效读取和精排放大。
- Dynamic scheduling 改善端到端和 tail latency。
- COCO100K 与 MS MARCO 实验支撑该系统设计。

## Figures and Tables Plan

| ID | Section | Content | Source |
|----|---------|---------|--------|
| Fig. 1 | Ch.1 | split pipeline vs BoundFetch-Guarded pipeline | conceptual |
| Fig. 2 | Ch.3/4 | SafeIn/SafeOut/Uncertain boundary | method |
| Fig. 3 | Ch.3 | candidate flow and read amplification | triage stats |
| Fig. 4 | Ch.7 | COCO recall@10-latency curve | main sweep |
| Fig. 5 | Ch.7 | MS MARCO recall@10-latency curve | main sweep |
| Fig. 6 | Ch.7 | triage ablation bars | ablation |
| Fig. 7 | Ch.7 | scheduling ablation p99 latency | ablation |
| Fig. 8 | Ch.7/Appendix | top-k sensitivity top20/top50/top100 | supplement |
| Table 1 | Ch.5 | system components | implementation |
| Table 2 | Ch.6 | dataset and run settings | experiment plan |
| Table 3 | Ch.7 | COCO matched-quality summary | main result |
| Table 4 | Ch.7 | MS MARCO matched-quality summary | main result |
| Table 5 | Ch.7 | FlatStor vs Lance benchmark summary | backend comparison |

## References To Include

### ANN and Quantization

1. Hervé Jégou, Matthijs Douze, Cordelia Schmid. **Product Quantization for Nearest Neighbor Search**. IEEE TPAMI 2011.  
   用途：PQ 与 IVF+PQ baseline。  
   Source: https://dblp.org/rec/journals/pami/JegouDS11

2. Jeff Johnson, Matthijs Douze, Hervé Jégou. **Billion-scale similarity search with GPUs**. arXiv 2017.  
   用途：Faiss 背景与大规模 similarity search。  
   Source: https://arxiv.org/abs/1702.08734

3. Matthijs Douze et al. **The Faiss library**. arXiv 2024.  
   用途：现代 Faiss 引用与 baseline 实现背景。  
   Source: https://arxiv.org/abs/2401.08281

4. Jianyang Gao, Cheng Long. **RaBitQ: Quantizing High-Dimensional Vectors with a Theoretical Error Bound for Approximate Nearest Neighbor Search**. SIGMOD 2024 / arXiv 2024.  
   用途：RaBitQ 误差界与量化估计来源。  
   Source: https://arxiv.org/abs/2405.12497

5. Yury A. Malkov, Dmitry A. Yashunin. **Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs**. TPAMI / arXiv.  
   用途：graph ANN 相关工作。  
   Source: https://arxiv.org/abs/1603.09320

6. Suhas Jayaram Subramanya et al. **DiskANN: Fast Accurate Billion-point Nearest Neighbor Search on a Single Node**. NeurIPS 2019.  
   用途：disk-aware ANN 相关工作。  
   Source: https://papers.nips.cc/paper_files/paper/2019/hash/09853c7fb1d3f8ee67a61b6bf4a7f8e6-Abstract.html

7. Qi Chen et al. **SPANN: Highly-efficient Billion-scale Approximate Nearest Neighbor Search**. NeurIPS 2021.  
   用途：memory/SSD hybrid ANN 相关工作。  
   Source: https://arxiv.org/abs/2111.08566

### Storage Backend and Vector Data Layout

8. Chi Zhang, Shihao Zhang, Yunfei Gu, Chentao Wu, Jie Li, Qin Zhang, Xusheng Chen, Jie Meng. **FlatStor: An Efficient Embedded-Index Based Columnar Data Layout for Multimodal Data Workloads**. PVLDB 19(1), 2025.  
   用途：FlatStor 主后端和 multimodal payload storage 背景。  
   Source: https://dblp.org/rec/journals/pvldb/ZhangZGWLZCM25  
   PDF: https://www.vldb.org/pvldb/vol19/p1-zhang.pdf

9. **Lance: Efficient Random Access in Columnar Storage through Adaptive Structural Encodings**. arXiv 2025.  
   用途：Lance benchmark backend，说明列式格式和随机访问优化。  
   Source: https://arxiv.org/abs/2504.15247

10. Lance documentation. **Lance Open Source Documentation**.  
    用途：Lance vector index、random access、multimodal format 的工程背景。  
    Source: https://lancedb.github.io/lance/index.html

11. LanceDB documentation. **Vector Search with ANN Index**.  
    用途：LanceDB/Lance 中 nprobes、refine/rerank、ANN index 行为说明。  
    Source: https://docs.lancedb.com/search/vector-search

### Datasets

12. Tsung-Yi Lin et al. **Microsoft COCO: Common Objects in Context**. ECCV 2014.  
    用途：COCO 数据集引用。  
    Source: https://publications.ri.cmu.edu/microsoft-coco-common-objects-in-context

13. Payal Bajaj et al. **MS MARCO: A Human Generated Machine Reading Comprehension Dataset**. arXiv 2016.  
    用途：MS MARCO Passage 数据集引用。  
    Source: https://microsoft.github.io/msmarco/Datasets.html

### I/O and Storage Engine Optimization

14. Matthias Jasny, Muhammad El-Hindi, Tobias Ziegler, Viktor Leis, Carsten Binnig. **High-Performance DBMSs with io_uring: When and How to use it**. arXiv 2025.  
    用途：io_uring 是否有效、如何使用 batch submit、registered buffers、polling 等设计依据；对应 `优化技巧.md` 第一部分。  
    Source: https://arxiv.org/abs/2512.04859

15. Gabriel Haas, Viktor Leis. **What Modern NVMe Storage Can Do, And How To Exploit It: High-Performance I/O for High-Performance Storage Engines**. PVLDB 16(9):2090-2102, 2023.  
    用途：NVMe 随机 I/O、批量提交、协作式调度、存储引擎 I/O 路径优化；对应 `优化技巧.md` 第二部分。  
    Source: https://www.vldb.org/pvldb/vol16/p2090-haas.pdf  
    DBLP: https://dblp.org/rec/journals/pvldb/HaasL23

16. LeanStore project page. **A High-Performance Storage Engine for Modern Hardware**.  
    用途：NVMe-optimized storage engine 背景，可作为 Haas/Leis 系列工作的工程上下文。  
    Source: https://www.cs.cit.tum.de/en/dis/research/leanstore/

## Ablation Experiments To Keep In Scope

### A1: Bound-Guided Triage Deletion

目的：证明 SafeIn/SafeOut/Uncertain 不是装饰性分类。

建议对比：

- full BoundFetch-Guarded
- remove SafeOut pruning
- remove SafeIn payload prefetch
- make Uncertain eager-fetch payload

主要指标：

- recall@10
- reranked candidates
- bytes read
- payload-prefetched reads
- final missing payload fetches
- p95/p99 latency

### A2: Boundary Sensitivity

目的：观察分类边界对 recall、read amplification 和 latency 的影响。

建议报告：

- SafeIn/SafeOut/Uncertain ratio
- recall@10
- E2E latency
- reranked candidates

这部分可以放主文一张小图，也可以放附录。

### A3: Dynamic Scheduling

目的：证明 I/O 侧优化对 tail latency 有贡献。

建议对比：

- submit-online on/off
- submit-batch sweep
- io-queue-depth sweep

主要指标：

- p95/p99 latency
- I/O wait time
- submit calls
- flush counts

### A4: Top-k Sensitivity

目的：回答更大返回集合下结论是否稳定。

建议顺序：

1. top20: 优先补，适合进入主文补充。
2. top50: 作为 stress。
3. top100: 如果 benchmark 输出和运行时间允许，放附录。

注意：不要在 benchmark 没有 recall@20/50/100 输出时提前写结论。

## Immediate Writing Plan

1. 先写第 3 章和第 4 章，因为它们不依赖最终数值，只依赖机制清楚。
2. 主实验结果出来后，先整理 recall@10-latency 曲线，再生成 matched-quality 表。
3. 消融实验完成后再写第 7 章后半部分；不要提前承诺哪个机制贡献最大。
4. 参考文献先按四类整理：ANN/quantization、storage backend、dataset、I/O optimization。
5. top20/top50/top100 只在实验输出支持后写进结果章节；否则保留为补充计划，不进入结论。
