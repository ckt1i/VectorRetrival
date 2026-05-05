# Experiment Plan: BoundFetch-Guarded 本科论文实验方案

**Problem**: 在本科毕业论文时间约束下，将 VectorRetrival/BoundFetch 收敛为一套最小但完整、可复现、可写作的实验闭环，证明 integrated vector retrieval + payload fetch 在真实 workload 上优于拆分式 baseline。  
**Method Thesis**: BoundFetch-Guarded 用 RaBitQ error-bound 将候选划分为 SafeIn / SafeOut / Uncertain，并用动态 io_uring 调度分别执行 vector+payload 预取、跳过、vector-only rerank，从而降低 matched-quality E2E latency。  
**Date**: 2026-05-03  
**Reference Settings**: `formal-baseline-study-2026-04-16` + `/home/zcq/VDB/baselines/formal-study/manifests/main_experiment_controls.json`  
**Post-Main-Sweep Status**: 主实验 T000-T015 已完成，后续计划正式切换为结果固化、机制消融和 top-k 补充验证。

**Ablation Freeze Status (2026-05-04)**: 机制消融、调度消融和 top20 补充已冻结到 `RESULT_FREEZE_ABLATION.md`。论文写作时主实验与消融分开叙述：主实验只使用 top10 retained main sweep；消融小节使用 triage/scheduling/top20 summary，不回写主表。

## Claim Map

| Claim | Why It Matters | Minimum Convincing Evidence | Linked Blocks |
|-------|-----------------|-----------------------------|---------------|
| C1: BoundFetch-Guarded 在多真实 workload 上降低 E2E latency | 这是主论文结论，必须避免 COCO-only 的偶然性 | COCO100K 与 MS MARCO 上，在 matched-quality 下优于 `IVF+PQ/IVF+RaBitQ x FlatStor/Lance` | B1, B2 |
| C2: 收益来自 SafeIn/SafeOut/Uncertain triage，而不是只来自 RaBitQ scorer | 隔离 dominant contribution | 关闭 SafeOut pruning、SafeIn prefetch 或 Uncertain delayed payload 后，bytes read、rerank candidates 或 E2E latency 变差 | B3 |
| C3: 动态 fetch scheduling 降低 tail latency | 证明系统调度不是普通异步 I/O 包装 | fixed submit / serial fetch 相比 dynamic policy 有更高 p95/p99 或 io wait | B4 |
| C4: top-k 放大下仍稳定 | 支撑鲁棒性与补充实验 | top20/top50/top100 下 BoundFetch-Guarded 仍控制 read amplification 和 tail latency | B5 |

## Paper Storyline

- Main paper must prove:
  - COCO100K 与 MS MARCO 两个真实 workload 上，BoundFetch-Guarded 在 matched-quality E2E latency 上优于拆分式 baseline。
  - SafeIn/SafeOut/Uncertain 三分类确实减少 rerank / payload read 放大。
  - 动态 submit policy 对 p95/p99 tail latency 有贡献。
- Appendix can support:
  - `topk=50` / `topk=100` stress test。
  - Lance 作为 benchmark/reference backend 的补充对比。
- Experiments intentionally cut:
  - DiskANN/HNSW/ScaNN 全量强 baseline。
  - Synthetic payload / Deep1M / Deep8M sensitivity，当前本科论文阶段不进入主线。
  - Amazon ESCI / LAION / audio / video 数据集。

## Frozen Protocol

### Dataset Priority

| Dataset | Status | Role | nlist | nprobe sweep | Query count | Notes |
|---------|--------|------|-------|--------------|-------------|-------|
| `coco_100k` | MUST | 图文跨模态主实验 | 2048 | 16,32,64,128,256,512 | 1000 | formal controls + precheck script 均支持 |
| `msmarco_passage` | MUST | 文本 dense retrieval 主实验 | 16384 | 16,32,64,128,256,512,1024 | 1000 | formal controls 已有历史 background run |

### Baseline Families

主实验只保留用户指定的四组合：

| Family | Backend | Role | Formal registry source |
|--------|---------|------|------------------------|
| `IVF+PQ+FlatStor` | FlatStor | PQ 拆分式 baseline | `faiss_ivfpq_refine_v1` |
| `IVF+RaBitQ+FlatStor` | FlatStor | 同量化路线 baseline | `official_rabitq_rerank_v1` |
| `IVF+PQ+Lance` | Lance | storage backend 对照 | `faiss_ivfpq_refine_v1 + lance payload` |
| `IVF+RaBitQ+Lance` | Lance | 同量化路线 + Lance | `official_rabitq_rerank_v1 + lance payload` |

BoundFetch-Guarded 作为主方法参与所有主表。DiskANN 只作为 appendix/reference，不进入本科最小闭环。

### Shared Parameters

- `queries=1000`
- `topk=10` 主矩阵
- `topk=20` COCO + MS MARCO 补充 operating point
- `topk=50` / `topk=100` COCO appendix stress（已 smoke 确认支持；完整矩阵 deferred）
- `bits=4` for IVF/PQ/RaBitQ-side official comparison
- `assignment-mode=single`
- BoundFetch 主实验使用 formal retained result set 中的执行参数；CRC/early-stop 只作为运行参数记录，不作为本文方法贡献。
- `clu-read-mode=full_preload` for primary warm-serving protocol
- `submission-mode=shared`
- `io-queue-depth=64`
- `prefetch-depth=16`
- `initial-prefetch=16`
- `refill-threshold=4`
- `refill-count=8`
- `submit-batch=8` for BoundFetch primary run, matching current precheck script
- `cluster-submit-reserve=8`

### BoundFetch Warmup Requirement

- Every formal BoundFetch measurement must be preceded by one same-configuration warmup run.
- Same-configuration means same dataset, canonical artifact, nlist, nprobe, topk, `crc`, `early-stop`, `bits`, `skip_gt`, scheduler settings, and triage variant flags.
- Warmup outputs are provenance only and must not enter paper tables, figures, matched-quality selection, or ablation summaries.
- If the warmup run is missing or fails, the following BoundFetch measurement is invalid for formal reporting.
- Track both run ids so the measurement can be traced to its warmup.

### Candidate Budget Policy

Formal controls define:

| topk | candidate_budget |
|------|------------------|
| 10 | 100 |
| 50 | 250 |
| 100 | 500 |

For this thesis plan:

- `topk=10`: use `candidate_budget=100`.
- `topk=20`: freeze `candidate_budget=150` for first pass; if no system reaches the common quality target, rerun with `candidate_budget=200` and record the change.
- `topk=50`: use `candidate_budget=250` if the deferred appendix stress matrix is later resumed.

Current freeze uses `topk=20` as the C4 sensitivity evidence. `topk=50/100` are not needed for the thesis-minimal main narrative.

BoundFetch-Guarded does not use a fixed top-C as the method mechanism, but candidate budget is still used to align baseline rerank work and top-k operating points.

### Output Layout

Keep outputs under the formal-study convention:

```text
/home/zcq/VDB/baselines/formal-study/outputs/
├── vector_search/{dataset}/{system}/{run_id}/metrics.csv
├── e2e/{dataset}/{system}/{run_id}/metrics.csv
├── build_cost/build_startup.csv
└── plots/
```

Thesis planning artifacts live under:

```text
/home/zcq/VDB/VectorRetrival/refine-logs/thesis-minimal-experiment-2026-05-02/
```

## Experiment Blocks

### Block 1: COCO100K Main E2E Result

- Claim tested: C1
- Why this block exists: COCO 是现有资产最成熟的图文跨模态 workload，必须先打穿完整方法 + baseline 闭环。
- Dataset / split / task: `coco_100k`, fixed 1000 query set, exact top100 GT if available; report top10 primarily.
- Compared systems:
  - BoundFetch-Guarded
  - `IVF+PQ+FlatStor`
  - `IVF+RaBitQ+FlatStor`
  - `IVF+PQ+Lance`
  - `IVF+RaBitQ+Lance`
- Metrics:
  - Primary: matched `recall@10`, avg/p50/p95/p99 E2E latency
  - Secondary: bytes read, SafeIn/SafeOut/Uncertain counts, reranked candidates, payload-prefetched reads, missing payload fetches
- Setup details:
  - `nlist=2048`
  - `nprobe=16,32,64,128,256,512`
  - `topk=10`, `candidate_budget=100` for baselines
  - BoundFetch: `bits=4`, `crc=0`, `assignment-mode=single`, `clu-read-mode=full_preload`, `submit-batch=8`
- Success criterion: BoundFetch-Guarded reaches the same or higher `recall@10` region with lower avg and p95/p99 E2E latency than both FlatStor baselines, and remains competitive against Lance variants.
- Failure interpretation: If only FlatStor wins but Lance loses, the story becomes “integrated layout + scheduler beats split FlatStor” and Lance moves to discussion; if all baselines win, triage/scheduler needs debugging before MS MARCO.
- Table / figure target:
  - Table 1: COCO matched-quality main table
  - Figure 1: recall@10 vs E2E latency curve
  - Figure 2: latency breakdown + triage stats
- Priority: DONE, now in result-freeze/reporting stage

### Block 2: MS MARCO Main E2E Result

- Claim tested: C1
- Why this block exists: 防止 COCO-only；MS MARCO 是文本 dense retrieval 主 benchmark。
- Dataset / split / task: `msmarco_passage`, fixed 1000 query set, exact top100 GT.
- Compared systems:
  - BoundFetch-Guarded
  - `IVF+PQ+FlatStor`
  - `IVF+RaBitQ+FlatStor`
  - `IVF+PQ+Lance`
  - `IVF+RaBitQ+Lance`
- Metrics:
  - Primary: `recall@10` + avg/p95/p99 E2E latency
  - Secondary: same system breakdown as COCO
- Setup details:
  - `nlist=16384`
  - `nprobe=16,32,64,128,256,512,1024`
  - `topk=10`, `candidate_budget=100`
  - Use same warm-serving protocol as COCO
- Success criterion: BoundFetch-Guarded shows E2E advantage on at least FlatStor baselines and does not regress quality.
- Failure interpretation: If MS MARCO gains are smaller, analyze passage payload size, SafeIn/SafeOut ratio, and tail-latency outliers.
- Table / figure target:
  - Table 2: MS MARCO matched-quality main table
  - Figure 3: COCO vs MS MARCO workload comparison in experiment section
- Priority: DONE, now in result-freeze/reporting stage

### Block 3: Bound-Guided Triage Ablation

- Claim tested: C2
- Why this block exists: 主实验已经证明 BoundFetch 更快；本 block 用 deletion study 证明收益来自 SafeIn/SafeOut/Uncertain 的候选级决策，而不是只来自整体实现或 RaBitQ scorer。
- Dataset / split / task: COCO100K primary；MS MARCO nprobe=128 可作为一个跨数据集补充点。
- Compared systems:
  - Full BoundFetch-Guarded
  - SafeOut pruning off
  - SafeIn payload prefetch off
  - Uncertain eager payload
  - Boundary sensitivity via `epsilon_percentile` / `override-eps-ip` / `override-d-k` if safe to run
  - Stage2 reclassification off/on only if existing flags are sufficient; otherwise appendix only
- Metrics:
  - SafeIn/SafeOut/Uncertain ratio
  - s2_safe_in / s2_safe_out
  - reranked candidates
  - payload-prefetched reads
  - missing payload fetches
  - bytes read
  - recall@10 and p95/p99 E2E latency
- Setup details:
  - Use COCO `nprobe=64` and/or `nprobe=128`; these correspond to useful mid/high-recall operating points in the retained main sweep.
  - Optional MS MARCO point: `nprobe=128`.
  - Keep `topk=10`, `queries=1000`, backend FlatStor first.
- Success criterion: Full policy reduces read/rerank amplification without harming recall; each deletion worsens at least one decisive metric.
- Failure interpretation: If ablation deltas are tiny, the main gain may be from scheduler or layout, and paper should reduce triage claim strength.
- Table / figure target:
  - Figure 4: triage ablation bars
  - Table 3: deletion study
- Priority: MUST-RUN

### Block 4: Dynamic Scheduling Ablation

- Claim tested: C3
- Why this block exists: Proves dynamic io_uring scheduling contributes to p95/p99 latency, not just ordinary async I/O.
- Dataset / split / task: COCO100K, FlatStor, selected operating point.
- Compared systems:
  - Full dynamic scheduling
  - `--submit-online 0` vs `1`
  - `submit-batch` sweep: 4, 8, 16, 32
  - `safein_all_threshold` sweep: disabled / default / large, as SHOULD-RUN
- Metrics:
  - avg/p95/p99 E2E latency
  - io_wait
  - uring_prep / uring_submit
  - submit calls
  - flush counts
  - bytes read and missing payload fetches
- Setup details:
  - Keep nprobe, topk, backend, and cache protocol fixed.
  - Use COCO `nprobe=64` or `nprobe=128`; do not repeat the full matrix unless the first result is ambiguous.
- Success criterion: Dynamic/default policy is near the Pareto front and lowers p95/p99 vs fixed or serial variants.
- Failure interpretation: If fixed `submit-batch` is equally good, simplify paper claim to “guarded selective fetch dominates; scheduler is implementation optimization.”
- Table / figure target:
  - Figure 5: p99 latency vs submit policy
  - Appendix table: scheduling parameter sweep
- Priority: MUST-RUN

### Block 5: Top-k Sensitivity

- Claim tested: C4
- Why this block exists: Demonstrates robustness when return set grows.
- Dataset / split / task:
  - COCO top-k sensitivity first
  - MS MARCO top-k sensitivity only if COCO top20 shows meaningful trend and output support is ready
- Compared systems:
  - BoundFetch-Guarded
  - `IVF+PQ+FlatStor`
  - `IVF+RaBitQ+FlatStor`
- Metrics:
  - topk=10/20/50/100 quality-latency
  - bytes read
  - reranked candidates
  - payload reads
  - p99 latency
- Setup details:
  - `topk=20`: `candidate_budget=150` first pass, rerun 200 if needed.
  - `topk=50`: `candidate_budget=250`.
  - `topk=100`: `candidate_budget=500`, appendix only.
  - Do not run larger top-k before benchmark or post-processing can report recall@20/50/100.
- Success criterion: BoundFetch-Guarded keeps lower read amplification as top-k increases.
- Failure interpretation: If top50 loses badly, keep top50 appendix and explain that large-k shifts bottleneck to required payload materialization.
- Table / figure target:
  - Appendix Figure A1: top-k sensitivity
  - Optional main-text supplement: top20 sensitivity
- Priority: SHOULD-RUN for top20, NICE-TO-HAVE for top50/top100

## Run Order and Milestones

| Milestone | Goal | Runs | Decision Gate | Cost | Risk |
|-----------|------|------|---------------|------|------|
| M0 | Asset and metric readiness | T000-T001 | 已完成 | DONE | none |
| M1 | Sanity runs | T002-T004, T010 | 已完成 | DONE | none |
| M2 | COCO main sweep | T005-T009 | 已完成，进入曲线/主表固化 | DONE | none |
| M3 | MS MARCO main sweep | T011-T015 | 已完成，需复核 nprobe=256 noisy point | DONE + 0.5 day | tail-latency outlier |
| M4 | Result hygiene | T028, T029 | matched-quality Q* 和代表点冻结 | 0.5 day | noisy point changes p99 narrative |
| M5 | Triage ablation | T016-T018, optional T019-T020 | Full policy has clear read/rerank or latency advantage | 1-2 days | Some ablations require small code flags |
| M6 | Scheduling ablation | T021-T023 | p95/p99 story clear | 0.5-1 day | effects may be smaller than triage |
| M7 | Top-k supplement | T024, optional T025/T030 | top20 robustness figure ready | 1-2 days | recall@20/50/100 output needs support |
| M8 | Paper polish | T027 | Main paper tables and ablations covered | 1-2 days | inconsistent output paths |

## Concrete Run Matrix

### MUST-RUN: COCO topk=10

| Run Group | Dataset | System | Backend | nlist | nprobe | topk | candidate_budget | Priority |
|-----------|---------|--------|---------|-------|--------|------|------------------|----------|
| C-MAIN-BF | coco_100k | BoundFetch-Guarded | integrated/FlatStor path | 2048 | 16,32,64,128,256,512 | 10 | n/a | MUST |
| C-MAIN-PQ-FS | coco_100k | IVF+PQ | FlatStor | 2048 | 16,32,64,128,256,512 | 10 | 100 | MUST |
| C-MAIN-RQ-FS | coco_100k | IVF+RaBitQ | FlatStor | 2048 | 16,32,64,128,256,512 | 10 | 100 | MUST |
| C-MAIN-PQ-LA | coco_100k | IVF+PQ | Lance | 2048 | 16,32,64,128,256,512 | 10 | 100 | SHOULD |
| C-MAIN-RQ-LA | coco_100k | IVF+RaBitQ | Lance | 2048 | 16,32,64,128,256,512 | 10 | 100 | SHOULD |

### MUST-RUN: MS MARCO topk=10

| Run Group | Dataset | System | Backend | nlist | nprobe | topk | candidate_budget | Priority |
|-----------|---------|--------|---------|-------|--------|------|------------------|----------|
| M-MAIN-BF | msmarco_passage | BoundFetch-Guarded | integrated/FlatStor path | 16384 | 16,32,64,128,256,512,1024 | 10 | n/a | MUST |
| M-MAIN-PQ-FS | msmarco_passage | IVF+PQ | FlatStor | 16384 | 16,32,64,128,256,512,1024 | 10 | 100 | MUST |
| M-MAIN-RQ-FS | msmarco_passage | IVF+RaBitQ | FlatStor | 16384 | 16,32,64,128,256,512,1024 | 10 | 100 | MUST |
| M-MAIN-PQ-LA | msmarco_passage | IVF+PQ | Lance | 16384 | 16,32,64,128,256,512,1024 | 10 | 100 | SHOULD |
| M-MAIN-RQ-LA | msmarco_passage | IVF+RaBitQ | Lance | 16384 | 16,32,64,128,256,512,1024 | 10 | 100 | SHOULD |

### POST-MAIN-SWEEP ADDENDUM: Result Hygiene

| Run Group | Dataset | System | Backend | Params | Priority | Purpose |
|-----------|---------|--------|---------|--------|----------|---------|
| R-HYGIENE-MSM256 | msmarco_passage | BoundFetch-Guarded | integrated/FlatStor path | nprobe=256,topk=10 | SHOULD | 复核当前 p99 异常点，避免 tail-latency 结论受 noisy point 影响 |
| R-HYGIENE-QSTAR | coco_100k + msmarco_passage | all retained systems | all retained backends | topk=10 | MUST | 冻结 matched-quality Q*、曲线点和论文主表代表点 |

### MUST-RUN: Post-Main-Sweep Triage Ablation

Run after Q* and representative operating points are frozen:

| Run Group | Dataset | Variant | Operating Point | Metrics | Priority |
|-----------|---------|---------|-----------------|---------|----------|
| A-TRI-SAFEOUT-OFF | coco_100k | SafeOut pruning off | nprobe=64 and/or 128,topk=10 | recall@10,e2e,p95,p99,reranked,bytes,triage ratio | MUST |
| A-TRI-SAFEIN-OFF | coco_100k | SafeIn payload prefetch off | nprobe=64 and/or 128,topk=10 | recall@10,e2e,p95,p99,payload_prefetch,missing_payload | MUST |
| A-TRI-UNCERTAIN-EAGER | coco_100k | Uncertain eager payload | nprobe=64 and/or 128,topk=10 | recall@10,e2e,p95,p99,bytes,payload_reads | MUST |
| A-TRI-MSM-REP | msmarco_passage | Full + most informative deletion | nprobe=128,topk=10 | same as COCO | SHOULD |

### MUST-RUN: Post-Main-Sweep Scheduling Ablation

Keep this matrix small and fixed to one representative COCO point unless the result is ambiguous:

| Run Group | Dataset | Variant | Operating Point | Metrics | Priority |
|-----------|---------|---------|-----------------|---------|----------|
| A-SCHED-ONLINE | coco_100k | submit-online on/off | nprobe=64 or 128,topk=10 | e2e,p95,p99,io_wait,submit_calls | MUST |
| A-SCHED-BATCH | coco_100k | submit-batch=4,8,16,32 | same as above | e2e,p95,p99,submit_calls,flushes | MUST |
| A-SCHED-SAFEIN-TH | coco_100k | safein_all_threshold disabled/default/large | same as above | e2e,p95,p99,queue pressure,bytes | SHOULD |

### SHOULD-RUN: topk=20 Supplement

Run only after matched top10 points are known:

- COCO: BoundFetch + two FlatStor baselines at topk=20, candidate_budget=150/200.
- MS MARCO: same if COCO top20 shows meaningful trend and output support is ready.
- Lance top20 only if Lance top10 is stable.

### NICE-TO-HAVE: topk=50/topk=100 Stress

- COCO topk=50 stress with candidate_budget=250.
- COCO topk=100 stress with candidate_budget=500.
- Do not run or report these until recall@50/100 output or post-processing is ready.

## Compute and Data Budget

- Total estimated GPU-hours: 0 if embeddings are already generated; otherwise MS MARCO embedding generation may dominate and should be handled outside this plan.
- CPU / I/O: main sweep is complete; remaining cost is a small number of fixed-point ablations and optional top-k supplement runs.
- Storage: expect formal baseline outputs under `/home/zcq/VDB/baselines/formal-study/outputs` and summary artifacts under `/home/zcq/VDB/baselines/results/formal-study/`.
- Human evaluation: none.
- Biggest bottleneck: ablation flags/proxies for SafeOut, SafeIn prefetch, and Uncertain eager payload.

## Risks and Mitigations

- Risk: MS MARCO `nprobe=256` retained point has abnormal p99.
  - Mitigation: rerun or mark as noisy before writing tail-latency claims.
- Risk: `recall@20/@50` not emitted by `bench_e2e`.
  - Mitigation: extend `recall_K` output or compute from per-query predictions; do not claim top20/top50/top100 until verified.
- Risk: ablation flags do not exist for SafeOut/SafeIn/Uncertain variants.
  - Mitigation: first use available proxy parameters (`safein_all_threshold`, stage2 flags, `override-eps-ip`, `override-d-k`, submit flags); only add minimal flags if proxy cannot isolate the claim.
- Risk: cold/warm protocols get mixed.
  - Mitigation: thesis main table uses warm-serving only; cold/drop-cache is appendix only.

## Final Checklist

- [x] COCO main sweep covered
- [x] MS MARCO main sweep covered
- [x] Four baseline combinations attempted
- [ ] Matched-quality rule applied consistently
- [ ] Triage novelty isolated
- [ ] Dynamic scheduling isolated
- [ ] top20/top50/top100 handled via benchmark extension or post-processing
- [ ] Nice-to-have runs separated from must-run runs
- [ ] Synthetic payload sensitivity marked out-of-scope for current本科论文
