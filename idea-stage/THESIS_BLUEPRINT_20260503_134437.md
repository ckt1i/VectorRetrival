# Thesis Blueprint: BoundFetch-Guarded

**Direction**: 基于 `thesis-minimal-experiment-2026-05-02` 的实验方案和当前 VectorRetrival 实现，同步推进本科毕业论文设计。  
**Date**: 2026-05-03  
**Inputs**:

- `refine-logs/thesis-minimal-experiment-2026-05-02/FINAL_PROPOSAL.md`
- `refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_PLAN.md`
- `refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_TRACKER.md`
- Current implementation: `ClusterProber`, `OverlapScheduler`, `SearchConfig`, `bench_e2e`

## Executive Summary

论文主线应收敛为一个系统型本科论文：**BoundFetch-Guarded 通过 RaBitQ 误差界驱动的候选三分类和异步读取调度，将 ANN 检索、exact rerank 与 payload fetch 从拆分式串行流程改造成 integrated E2E pipeline，从而在 matched-quality 下降低 payload-rich retrieval 的端到端延迟。**

当前实验进展已经足够支撑论文构思：COCO 和 MS MARCO 的主实验资产与主 sweep 已在 tracker 中标为完成，后续写作风险主要不在“能不能跑主实验”，而在 **mechanism isolation 是否足够扎实**。因此下一阶段优先级应从继续扩大 baseline 转向补齐消融和评估脚手架。

最关键的实现口径需要在论文中保持严格：

- 不启用 ConANN CRC early stop / RAIR / duplicate assignment 作为论文主贡献。
- 但如果要主张 SafeOut pruning，当前代码需要保留某种 calibrated bound / estimate frontier 来源；否则 `--crc 0` 下 `dynamic_d_k = +inf` 会让 SafeOut 基本失效。
- 推荐论文表述为：**CRC early stop is disabled, while calibrated RaBitQ-bound statistics are used only to form conservative triage thresholds.** 也就是“关闭 CRC 停止决策”，不是“关闭所有 CRC/calibration 参数”。

## Current Implementation Reading

### Method Components in Code

| Paper concept | Current code path | What to write |
|---------------|-------------------|---------------|
| IVF probing and coarse selection | `OverlapScheduler::Search` -> `FindNearestClusters` | 先选择 `nprobe` 个最近 cluster，论文不把 cluster assignment 作为创新 |
| RaBitQ distance estimation | `ClusterProber::Probe`, `rabitq_estimator` | 用快速估计距离和 bound/margin 判断候选类别 |
| SafeOut pruning | `ClusterProber::Probe`, `FastScanSafeOutMask` | 估计距离超过动态上界的候选跳过，不读取原向量和 payload |
| SafeIn prefetch | `FastScanSafeInMask` + `AsyncIOSink::ScanAndPartitionBatch` | 高置信候选直接进入 vector+payload read，但受 `safein_all_threshold` 限制 |
| Uncertain delayed payload | `AsyncIOSink` -> `VEC_ONLY`, `RerankConsumer`, `FetchMissingPayloads` | 边界候选只读原向量做 rerank，最终进入 top-k 后补读 payload |
| Dynamic async scheduling | `ProbeAndDrainInterleaved`, `EmitPendingDataRequests` | 根据 pending requests、queue depth、submit batch、probe 进度动态提交 io_uring |
| Metrics export | `bench_e2e.cpp::RoundMetrics` and JSON output | 已有 recall@1/5/10、latency breakdown、triage stats、submit stats |

### Implementation Caveats to Respect

1. `SearchConfig` 默认 `early_stop=true`，但 thesis main protocol 应显式运行 `--early-stop 0` 或确保 CRC stopper 不 break。
2. `--crc 0` 会让 `use_crc_ = false`，`dynamic_d_k` 在 `ProbeCluster` 中保持 `+inf`。这意味着 SafeOut 依赖的动态阈值不会生效。论文主实验若声称 SafeOut pruning，必须检查真实 run 是否使用 `--crc 1 --early-stop 0` 或等价路径。
3. SafeIn 当前使用 `conann_.d_k()` 作为静态 conservative base；SafeOut 使用动态 `dynamic_d_k`。论文不要写成 SafeIn 和 SafeOut 都完全围绕同一个实时 top-k 半径。
4. `safein_all_threshold` 的语义是 `addr.size <= threshold` 时 SafeIn 走 `VEC_ALL`。因此禁用 SafeIn payload prefetch 的 proxy 应是 threshold 设得很小，例如 `0`；强制全部 SafeIn 预取则是设为大于最大 payload size。
5. `bench_e2e` 支持 `--topk`，但当前 `RoundMetrics` 固定输出 `recall_at_1/5/10`。论文若报告 recall@20/@50，需要扩展 benchmark 或做后处理。

## Thesis Claims

### Primary Claim

BoundFetch-Guarded 在 COCO100K 和 MS MARCO 两个真实 workload 上，在相同 recall@10 / matched-quality 条件下降低端到端查询延迟，特别是 p95/p99 latency。

### Mechanism Claim

主要收益来自 bound-guided candidate triage：

- SafeOut 减少原向量读取和 rerank 候选数。
- SafeIn 在低风险时提前读取 payload，减少最终 top-k materialization 等待。
- Uncertain 延迟读取 payload，避免边界候选的 payload read amplification。

### Systems Claim

动态 io_uring submit / drain 策略能把 cluster probe、vector read、payload read、rerank 重叠起来，降低 tail latency，而不只是把同步读取替换为普通异步读取。

### Scope Claim

本文不声称击败所有 ANN 系统，不把 graph index、DiskANN、HNSW、ScaNN 或商业向量数据库作为主 baseline。论文目标是证明在 IVF + compressed-distance + payload-rich retrieval 这个受控设计空间中，bound-guided integrated fetch 明显优于拆分式 pipeline。

## Detailed Thesis Outline

### 第 1 章 引言

**目标**: 用系统问题引出本文，而不是从“我实现了一个向量库”开始。

建议小节：

1.1 背景：embedding retrieval 已成为多模态检索、RAG、问答系统的基础组件。  
1.2 问题：实际服务不是只返回 vector id，而是需要 payload materialization 和 rerank；拆分式 ANN + storage fetch 会引入额外 E2E latency。  
1.3 观察：候选并不等价，有些几乎必进 top-k，有些几乎不可能进入 top-k，边界候选才需要 expensive exact rerank。  
1.4 方法概述：BoundFetch-Guarded 使用 RaBitQ bound 将候选分为 SafeIn/SafeOut/Uncertain，并为三类候选选择不同 I/O 策略。  
1.5 贡献：

- 提出 bound-guided selective fetch pipeline。
- 实现动态 io_uring 调度以重叠 probe、fetch、rerank。
- 在 COCO100K 与 MS MARCO 上进行 matched-quality E2E 评估，并补充消融。

建议图：

- Figure 1: Split pipeline vs BoundFetch-Guarded integrated pipeline。

### 第 2 章 背景与相关工作

建议小节：

2.1 Approximate Nearest Neighbor Search: IVF、graph index、disk-aware ANN。  
2.2 Vector Quantization: PQ、IVF-PQ、Faiss、RaBitQ。  
2.3 Vector Databases and Payload Fetch: FlatStor、Lance、payload materialization。  
2.4 Asynchronous I/O for Data Systems: io_uring、queue depth、batch submit、tail latency。  
2.5 本文定位：不是新量化器，不是新图索引，而是用 error bound 驱动 E2E fetch decision。

写作要点：

- PQ 和 Faiss 是 baseline 背景。
- RaBitQ 是本文使用的 bound source，需要明确“借用其理论 error-bound，不声称重新提出量化器”。
- DiskANN/SPANN 用于说明 disk-aware ANN 的重要性，但主 baseline 不引入它们，原因是论文受控变量聚焦于 `IVF+PQ/RaBitQ x payload backend`。

### 第 3 章 问题定义与动机

建议小节：

3.1 Query Model: 输入 query vector，输出 top-k records，包括 vector distance 和 payload。  
3.2 Cost Model: coarse probe、compressed scan、exact vector read、payload read、rerank、missing payload fetch。  
3.3 Matched-Quality Evaluation: 给定质量阈值 `Q*`，比较达到 `Q*` 的最低 E2E latency。  
3.4 Motivation Study: 使用 COCO sanity 或已完成主实验中的一两个 operating point 展示：

- SafeOut ratio
- Uncertain ratio
- reranked candidates
- payload-prefetched vs missing payload fetched
- baseline 的 fetch/rerank 放大

建议图表：

- Figure 2: 三界限分类示意图，展示 estimated distance 与 `d_k +/- margin`。
- Figure 3: 一个 query 的候选流向 Sankey / stacked bar。

注意：

- Workload 差异不要放在第 3 章作为独立大段；放到第 6 章实验设置更自然。第 3 章只保留“为什么 E2E payload-rich retrieval 是问题”的抽象定义和动机数据。

### 第 4 章 方法：BoundFetch-Guarded

建议小节：

4.1 Overview: pipeline 组件和数据流。  
4.2 Bound-Guided Candidate Triage:

- RaBitQ estimated distance
- margin / epsilon interpretation
- SafeIn、SafeOut、Uncertain 判定
- 当前实现口径：conservative static SafeIn + dynamic SafeOut

4.3 Fetch Policy:

- SafeIn -> `VEC_ALL` if payload size <= threshold
- SafeOut -> skip
- Uncertain -> `VEC_ONLY` then exact rerank; final top-k missing payload fetch

4.4 Dynamic I/O Scheduling:

- pending_all_plans and pending_vec_only_plans
- queue depth and cluster reserve
- hard/soft/tail flush
- online submit tuning

4.5 Correctness and Safety:

- SafeOut 可能影响 recall，因此必须通过 margin/calibration 控制，并以 recall@10 作为硬约束。
- SafeIn 错误不会丢结果，只会造成提前读取 payload 的浪费；Uncertain 保守处理保证 rerank。

建议算法框：

- Algorithm 1: BoundFetch-Guarded Search
- Algorithm 2: Candidate Triage and Read Plan Generation

### 第 5 章 系统实现

建议小节：

5.1 Storage Layout: cluster blocks, address column, raw vector/payload region。  
5.2 Query Execution Engine: `OverlapScheduler` 搜索流程。  
5.3 Candidate Classification: `ClusterProber` 的 Stage 1 FastScan 和 Stage 2 ExRaBitQ reclassification。  
5.4 Async Read Path: `AsyncIOSink`, `VEC_ONLY`, `VEC_ALL`, buffer pool, io_uring reader。  
5.5 Metrics and Benchmark Harness: `bench_e2e` 输出哪些字段，如何聚合 p50/p95/p99。  
5.6 Implementation Boundary:

- 主线 single assignment。
- 不启用 RAIR / duplicate clustering。
- 不启用 CRC early stop；若使用 CRC/calibration 参数，只用于形成保守 bound frontier。

建议表：

- Table 1: 关键参数及默认值：`nprobe`, `top_k`, `io_queue_depth`, `submit_batch`, `safein_all_threshold`, `prefetch_depth`, `initial_prefetch`, `refill_threshold/refill_count`。

### 第 6 章 实验设置

建议小节：

6.1 Research Questions:

- RQ1: BoundFetch 是否在真实 workload 上降低 matched-quality E2E latency？
- RQ2: SafeIn/SafeOut/Uncertain 哪些机制贡献最大？
- RQ3: 动态 fetch scheduling 是否降低 tail latency？
- RQ4: top-k 或 payload size 增大时结论是否稳定？

6.2 Datasets:

- COCO100K: 图文/多模态 workload。
- MS MARCO Passage: 文本 dense retrieval workload。
- Deep1M_synth / Deep8M_synth / COCO padded payload: payload sensitivity optional。

6.3 Baselines:

- IVF+PQ+FlatStor
- IVF+RaBitQ+FlatStor
- IVF+PQ+Lance
- IVF+RaBitQ+Lance

6.4 Protocol:

- `queries=1000`
- COCO: `nlist=2048`, `nprobe=16,32,64,128,256,512`
- MS MARCO: `nlist=16384`, `nprobe=16,32,64,128,256,512,1024`
- `topk=10` main, `topk=20` supplement, `topk=50` appendix stress
- warm-serving main protocol

6.5 Metrics:

- recall@10, optional MRR@10/nDCG@10 for MS MARCO if scorer is wired
- avg/p50/p95/p99 E2E latency
- bytes read, reranked candidates
- SafeIn/SafeOut/Uncertain ratio
- payload_prefetched, missing_payload_fetch
- submit calls, flush counts, io_wait

6.6 Matched-Quality Rule:

- 对每个 dataset，先找所有主线系统共同可达的质量阈值 `Q*`。
- 每个系统报告达到 `Q*` 的最低 latency。
- 不 cherry-pick 单点；曲线和主表都保留。

### 第 7 章 实验结果

建议结构：

7.1 Main Result on COCO100K:

- Quality-latency curve
- matched-quality table
- latency breakdown

7.2 Main Result on MS MARCO:

- 同 COCO
- 若 MRR/nDCG 未完成，主表先以 ANN recall@10 为质量控制，相关性指标放补充/待补。

7.3 Triage Ablation:

- Full vs SafeOut off vs SafeIn prefetch off vs Uncertain eager payload
- boundary sensitivity
- Stage2 contribution

7.4 Scheduling Ablation:

- submit-online on/off
- submit-batch sweep
- threshold sweep
- no-overlap / serial fetch if implemented

7.5 Top-k and Payload Sensitivity:

- topk=20 main supplement
- topk=50 appendix stress
- payload 256B/4KB/64KB if synthetic ready

7.6 Summary:

- 逐个回答 RQ1-RQ4。

### 第 8 章 讨论与局限

建议内容：

- 为什么不纳入 DiskANN/HNSW/ScaNN 主 baseline：设计空间不同，且本科论文资源有限。
- 为什么不启用 RAIR/duplicate clustering：会混入 cluster redundancy 变量，掩盖 triage/fetch 机制。
- CRC early stop 的边界：本文不评估 early stop，只使用或替代其 calibration 作为保守 bound frontier。
- SafeOut 的 recall 风险和 calibration 需求。
- 大 top-k 下 selective fetch 收益可能下降，因为最终 materialized payload 本身增多。
- Lance runner 或 qrels scorer 若不稳定，应诚实进入 threats to validity。

### 第 9 章 结论

用一段总结即可：

- Bound-guided selective fetch 把候选级置信度转化为 I/O 策略。
- Dynamic scheduling 将 probe/fetch/rerank overlap。
- 两个真实 workload 和机制消融支持本文结论。

## Figures and Tables Checklist

| ID | Target | Content | Source |
|----|--------|---------|--------|
| Fig. 1 | Introduction | split pipeline vs integrated BoundFetch pipeline | conceptual |
| Fig. 2 | Motivation / Method | SafeIn/SafeOut/Uncertain boundary diagram | method |
| Fig. 3 | Motivation | candidate flow: probed -> SafeOut/SafeIn/Uncertain -> rerank -> payload | bench stats |
| Fig. 4 | Evaluation | COCO recall@10 vs E2E latency curve | T005-T009 |
| Fig. 5 | Evaluation | MS MARCO recall@10 vs E2E latency curve | T011-T015 |
| Fig. 6 | Ablation | triage deletion bars | T016-T020 |
| Fig. 7 | Ablation | p99 latency vs submit policy | T021-T023 |
| Fig. A1 | Appendix | topk=20/50 sensitivity | T024-T025 |
| Fig. A2 | Appendix | payload size sensitivity | T026 |
| Table 1 | Implementation | parameters and defaults | code/config |
| Table 2 | Evaluation | dataset and benchmark settings | experiment plan |
| Table 3 | Evaluation | COCO matched-quality summary | main results |
| Table 4 | Evaluation | MS MARCO matched-quality summary | main results |
| Table 5 | Ablation | deletion study summary | ablations |

## Required References

### Core ANN and Quantization

1. Jégou, Douze, Schmid. **Product Quantization for Nearest Neighbor Search**. IEEE TPAMI 2011.  
   Use for: PQ and IVF+PQ baseline background.  
   Source: https://dblp.org/rec/journals/pami/JegouDS11

2. Johnson, Douze, Jégou. **Billion-scale similarity search with GPUs**. arXiv 2017.  
   Use for: Faiss / compressed-domain search / billion-scale similarity search background.  
   Source: https://arxiv.org/abs/1702.08734

3. Douze et al. **The Faiss library**. arXiv 2024, latest arXiv revision seen in 2025.  
   Use for: modern Faiss citation and baseline implementation context.  
   Source: https://arxiv.org/abs/2401.08281

4. Gao and Long. **RaBitQ: Quantizing High-Dimensional Vectors with a Theoretical Error Bound for Approximate Nearest Neighbor Search**. SIGMOD 2024 / arXiv 2024.  
   Use for: theoretical error-bound source and RaBitQ baseline.  
   Source: https://arxiv.org/abs/2405.12497

### Disk / Hybrid ANN Background

5. Subramanya et al. **DiskANN: Fast Accurate Billion-point Nearest Neighbor Search on a Single Node**. NeurIPS 2019.  
   Use for: disk-aware ANN motivation and related work, not main baseline.  
   Source: https://papers.nips.cc/paper_files/paper/2019/hash/09853c7fb1d3f8ee67a61b6bf4a7f8e6-Abstract.html

6. Chen et al. **SPANN: Highly-efficient Billion-scale Approximate Nearest Neighbor Search**. NeurIPS 2021.  
   Use for: inverted-index style memory/SSD hybrid ANN and disk-access reduction.  
   Source: https://arxiv.org/abs/2111.08566

7. Malkov and Yashunin. **Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs**. arXiv 2016 / TPAMI version later.  
   Use for: graph ANN related work and why graph baselines are outside main controlled matrix.  
   Source: https://arxiv.org/abs/1603.09320

### Datasets and Storage Backends

8. Lin et al. **Microsoft COCO: Common Objects in Context**. ECCV 2014.  
   Use for: COCO dataset citation.  
   Source: https://publications.ri.cmu.edu/microsoft-coco-common-objects-in-context

9. Bajaj et al. **MS MARCO: A Human Generated Machine Reading Comprehension Dataset**. arXiv 2016.  
   Use for: MS MARCO dataset citation.  
   Source: https://microsoft.github.io/msmarco/Datasets.html

10. Lance documentation. **Lance: columnar data format optimized for ML and AI applications**.  
    Use for: Lance backend description; cite as software/documentation if no paper is required by your thesis format.  
    Source: https://lancedb.github.io/lance/index.html

### I/O and Systems Background

11. Didona et al. **Understanding Modern Storage APIs: A systematic study of libaio, SPDK, and io-uring**. SYSTOR 2022.  
    Use for: io_uring and storage API background.  
    Source: https://research.ibm.com/publications/understanding-modern-storage-apis-a-systematic-study-of-libaio-spdk-and-io-uring

12. Lutz et al. **High-Performance DBMSs with io_uring: When and How to use it**. arXiv 2025.  
    Use for: modern DBMS io_uring design discussion; optional because it is recent and may be less established.  
    Source: https://arxiv.org/abs/2512.04859

## Ablation Gaps and Recommended Fixes

### A0: Clarify CRC / Bound Frontier Mode

**Problem**: 当前计划多处写 `crc=0`，但代码中 `--crc 0` 会让 `dynamic_d_k = +inf`，削弱 SafeOut。  
**Recommendation**:

- 主实验采用 `--crc 1 --early-stop 0` 或等价配置。
- 论文表述为“不启用 CRC early stop，只使用 calibrated bound frontier for triage”。
- 增加一个 sanity ablation:
  - `bound_frontier=on, early_stop=off`
  - `bound_frontier=off` 即当前 `--crc 0`
  - 观察 SafeOut ratio、reranked candidates、recall@10、latency

如果不想继续使用 CRC 命名，建议代码上增加更清晰的 flag，例如：

- `--bound-frontier 0/1`
- `--early-stop 0/1`

### A1: SafeOut Pruning Off

**Current support**: 没有直接 flag。  
**Minimal implementation**:

- `SearchConfig::enable_safeout_pruning = true`
- `bench_e2e --safeout-pruning 0`
- 在 `ClusterProber::Probe` 或调用处将 SafeOut mask 置零。

**Metrics**:

- SafeOut ratio should become 0
- reranked candidates should increase
- recall should not decrease
- E2E/p99 likely increases if SafeOut is useful

### A2: SafeIn Payload Prefetch Off / Threshold Sweep

**Current support**: 可用 `safein_all_threshold` proxy，但 CLI 可能需要暴露。  
**Recommended runs**:

- `safein_all_threshold=0`: disable SafeIn payload prefetch
- `safein_all_threshold=4KB`: small payload only
- `safein_all_threshold=256KB`: current default
- `safein_all_threshold=large`: force all SafeIn payload prefetch

**Metrics**:

- payload_prefetched
- missing_payload_fetch
- bytes read
- p95/p99 latency

### A3: Uncertain Eager Payload

**Current support**: 没有直接 flag。  
**Minimal implementation**:

- `SearchConfig::uncertain_read_all = false`
- `bench_e2e --uncertain-read-all 1`
- 在 `AsyncIOSink::ScanAndPartitionBatch` 中，把 Uncertain 也加入 `VEC_ALL`，但仍做 rerank。

**Purpose**:

- 证明 Uncertain delayed payload 避免错误 payload prefetch。

### A4: Stage2 Reclassification On / Off

**Current support**:

- 有 `--stage2-block-first`
- 有 `--stage2-batch-classify`
- 但没有完全 disable Stage2 的清晰 flag。

**Recommendation**:

- 若只想写系统优化，不把 Stage2 作为核心 claim，可以把它放 appendix。
- 若要作为机制消融，建议加 `--stage2-reclassify 0/1`。
- 不建议用 `bits=1 vs bits=4` 代替 Stage2 消融，因为这同时改变了 quantization setting。

### A5: Dynamic Scheduling Ablation

**Current support**:

- `--submit-online`
- `--submit-batch`
- `--io-queue-depth`
- `--cluster-submit-reserve`

**Missing but useful**:

- `--no-overlap-fetch` or `--serial-fetch-after-probe`

**Recommended minimum**:

- Keep existing submit-online on/off and submit-batch sweep as main.
- Add no-overlap only if implementation cost small; otherwise在论文里承认“serial fetch path is approximated by large submit batch / no online tuning”。

### A6: Recall@20 / Recall@50 Output

**Problem**: `bench_e2e` 当前输出固定 `recall_at_1/5/10`。  
**Minimal implementation**:

- Add dynamic recall set: `{1,5,10,20,50}` clipped by `top_k` and GT depth。
- Or write post-processing over per-query predicted ids and GT top100。

**Recommendation**:

- topk=20 应该进入主文补充。
- topk=50 只作为 appendix stress，不要拖慢主线。

### A7: Payload Size Sensitivity

**Priority**: Should-run if主实验已经稳定；否则 appendix。  
**Recommended variants**:

- 256B
- 4KB
- 64KB

**Fallback**:

- 如果 `deep1m_synth` 未生成，用 `deep8m_synth` 或 COCO payload padding。

## Updated Experiment Priority

| Priority | Experiment | Why |
|----------|------------|-----|
| P0 | Confirm main run mode: bound frontier on, early stop off | 避免 SafeOut claim 与 `--crc 0` 实现矛盾 |
| P0 | SafeOut pruning off | 最直接证明 pruning 贡献 |
| P0 | SafeIn prefetch off / threshold sweep | 最直接证明 payload prefetch 贡献 |
| P0 | Uncertain eager payload | 证明 delayed payload 的必要性 |
| P1 | submit-online on/off and submit-batch sweep | 支撑 dynamic scheduling claim |
| P1 | recall@20 support + topk=20 | 回答更大 top-k 普适性问题 |
| P2 | Stage2 off/on | 如果篇幅允许，作为 appendix 或实现优化 |
| P2 | payload size sensitivity | 如果 synthetic 数据准备完成 |
| P3 | topk=50 | appendix stress only |

## Paper-Writing Plan After Main Results Finish

1. Freeze result schema:
   - all run ids
   - dataset version
   - exact command template
   - output json/csv path

2. Build matched-quality table:
   - per dataset select `Q*`
   - for each system choose minimum E2E latency satisfying `Q*`
   - keep full recall-latency curves in figures

3. Write evaluation first:
   - fill Section 6 and 7 before polishing intro
   - claims must match observed deltas

4. Then write method:
   - only describe implemented mechanism
   - avoid overclaiming dynamic SafeIn
   - explicitly state RAIR/CRC early stop are out of scope

5. Finalize related work:
   - cite PQ/Faiss/RaBitQ for quantization
   - cite DiskANN/SPANN/HNSW for ANN systems
   - cite COCO/MS MARCO for datasets
   - cite io_uring storage papers for asynchronous I/O context

## Threats to Validity

- COCO100K is a reduced dataset; MS MARCO helps, but Deep1M/Deep8M synthetic only tests payload behavior, not necessarily semantic retrieval diversity.
- BoundFetch is evaluated in an IVF-family design space; graph ANN baselines are related work, not direct competitors.
- If Lance backend results are unstable, conclusions should focus on FlatStor and discuss Lance as backend sensitivity.
- If MRR@10/nDCG@10 are not wired for MS MARCO, do not present them as completed; use ANN recall@10 until qrels scorer is verified.
- Warm-serving and cold-cache protocols must not be mixed in the same main table.

## Immediate Next Actions

1. Check whether completed BoundFetch main runs used `--crc 1 --early-stop 0` or `--crc 0`; update tracker wording from `crc=0` to `bound_frontier=on/off` if necessary.
2. Add or expose minimal ablation flags for SafeOut off and Uncertain eager payload.
3. Run COCO ablations at the matched-quality nprobe before extending more datasets.
4. Extend recall@20 output or prepare post-processing before topk=20 runs.
5. After P0/P1 ablations, start drafting Sections 3-5 with code-grounded details.
