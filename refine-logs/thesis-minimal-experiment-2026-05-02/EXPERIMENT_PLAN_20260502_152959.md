# Experiment Plan: BoundFetch-Guarded 本科论文实验方案

**Problem**: 在本科毕业论文时间约束下，将 VectorRetrival/BoundFetch 收敛为一套最小但完整、可复现、可写作的实验闭环，证明 integrated vector retrieval + payload fetch 在真实 workload 上优于拆分式 baseline。  
**Method Thesis**: BoundFetch-Guarded 用 RaBitQ error-bound 将候选划分为 SafeIn / SafeOut / Uncertain，并用动态 io_uring 调度分别执行 vector+payload 预取、跳过、vector-only rerank，从而降低 matched-quality E2E latency。  
**Date**: 2026-05-02  
**Reference Settings**: `formal-baseline-study-2026-04-16` + `/home/zcq/VDB/baselines/formal-study/manifests/main_experiment_controls.json`

## Claim Map

| Claim | Why It Matters | Minimum Convincing Evidence | Linked Blocks |
|-------|-----------------|-----------------------------|---------------|
| C1: BoundFetch-Guarded 在多真实 workload 上降低 E2E latency | 这是主论文结论，必须避免 COCO-only 的偶然性 | COCO100K 与 MS MARCO 上，在 matched-quality 下优于 `IVF+PQ/IVF+RaBitQ x FlatStor/Lance` | B1, B2 |
| C2: 收益来自 SafeIn/SafeOut/Uncertain triage，而不是只来自 RaBitQ scorer | 隔离 dominant contribution | 关闭 SafeOut pruning、SafeIn prefetch 或 Uncertain delayed payload 后，bytes read、rerank candidates 或 E2E latency 变差 | B3 |
| C3: 动态 fetch scheduling 降低 tail latency | 证明系统调度不是普通异步 I/O 包装 | fixed submit / serial fetch 相比 dynamic policy 有更高 p95/p99 或 io wait | B4 |
| C4: top-k 和 payload size 放大下仍稳定 | 支撑鲁棒性与 appendix | top20/top50 或 synthetic payload size 增大时，BoundFetch-Guarded 仍控制 read amplification | B5 |

## Paper Storyline

- Main paper must prove:
  - COCO100K 与 MS MARCO 两个真实 workload 上，BoundFetch-Guarded 在 matched-quality E2E latency 上优于拆分式 baseline。
  - SafeIn/SafeOut/Uncertain 三分类确实减少 rerank / payload read 放大。
  - 动态 submit policy 对 p95/p99 tail latency 有贡献。
- Appendix can support:
  - `topk=50` stress test。
  - Deep1M/Deep8M synthetic payload size sensitivity。
  - Lance 若 runner 不稳定，可从主表降级为 appendix risk note。
- Experiments intentionally cut:
  - CRC early stop。
  - RAIR / duplicate clustering。
  - DiskANN/HNSW/ScaNN 全量强 baseline。
  - Amazon ESCI / LAION / audio / video 数据集。

## Frozen Protocol

### Dataset Priority

| Dataset | Status | Role | nlist | nprobe sweep | Query count | Notes |
|---------|--------|------|-------|--------------|-------------|-------|
| `coco_100k` | MUST | 图文跨模态主实验 | 2048 | 16,32,64,128,256,512 | 1000 | formal controls + precheck script 均支持 |
| `msmarco_passage` | MUST | 文本 dense retrieval 主实验 | 16384 | 16,32,64,128,256,512,1024 | 1000 | formal controls 已有历史 background run |
| `deep1m_synth` | NICE | payload size sensitivity | TBD | 64,128,256 initially | 1000 | 用户目标；当前本地 formal path 看到 `deep8m_synth`，执行前需确认 |
| `deep8m_synth` | BACKUP | synthetic payload fallback | 12800 | 16,32,64,128,256,512 | 1000 | formal controls active，可作为 deep1m_synth 未就绪时替代 |

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
- `topk=50` COCO 或 synthetic appendix stress
- `bits=4` for IVF/PQ/RaBitQ-side official comparison
- `assignment-mode=single`
- `crc=0`
- `clu-read-mode=full_preload` for primary warm-serving protocol
- `submission-mode=shared`
- `io-queue-depth=64`
- `prefetch-depth=16`
- `initial-prefetch=16`
- `refill-threshold=4`
- `refill-count=8`
- `submit-batch=8` for BoundFetch primary run, matching current precheck script
- `cluster-submit-reserve=8`

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
- `topk=50`: use `candidate_budget=250`.

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
- Priority: MUST-RUN

### Block 2: MS MARCO Main E2E Result

- Claim tested: C1
- Why this block exists: 防止 COCO-only；MS MARCO 是文本 dense retrieval 主 benchmark。
- Dataset / split / task: `msmarco_passage`, fixed 1000 query set, exact top100 GT and qrels if available.
- Compared systems:
  - BoundFetch-Guarded
  - `IVF+PQ+FlatStor`
  - `IVF+RaBitQ+FlatStor`
  - `IVF+PQ+Lance`
  - `IVF+RaBitQ+Lance`
- Metrics:
  - Primary: `recall@10` + avg/p95/p99 E2E latency
  - If qrels ready: `MRR@10`, `nDCG@10`
  - Secondary: same system breakdown as COCO
- Setup details:
  - `nlist=16384`
  - `nprobe=16,32,64,128,256,512,1024`
  - `topk=10`, `candidate_budget=100`
  - Use same warm-serving protocol as COCO
- Success criterion: BoundFetch-Guarded shows E2E advantage on at least FlatStor baselines and does not regress quality.
- Failure interpretation: If MS MARCO gains are smaller, analyze passage payload size and SafeIn/SafeOut ratio; if qrels block progress, use ANN recall first and mark qrels metrics as delayed.
- Table / figure target:
  - Table 2: MS MARCO matched-quality main table
  - Figure 3: COCO vs MS MARCO workload comparison in experiment section
- Priority: MUST-RUN

### Block 3: Bound-Guided Triage Ablation

- Claim tested: C2
- Why this block exists: Isolates SafeIn/SafeOut/Uncertain as the dominant mechanism.
- Dataset / split / task: COCO100K primary; one matched operating point on MS MARCO if time allows.
- Compared systems:
  - Full BoundFetch-Guarded
  - SafeOut pruning off
  - SafeIn payload prefetch off
  - Uncertain eager payload
  - Stage2 reclassification off/on if existing flags are sufficient
  - Boundary sensitivity via `epsilon_percentile` / `override-eps-ip` / `override-d-k` if safe to run
- Metrics:
  - SafeIn/SafeOut/Uncertain ratio
  - s2_safe_in / s2_safe_out
  - reranked candidates
  - payload-prefetched reads
  - missing payload fetches
  - bytes read
  - recall@10 and p95/p99 E2E latency
- Setup details:
  - Use COCO `nprobe` nearest to matched-quality operating point, likely 128 or 256 after Block 1.
  - Keep `topk=10`, `queries=1000`, backend FlatStor first.
- Success criterion: Full policy reduces read/rerank amplification without harming recall; each deletion worsens at least one decisive metric.
- Failure interpretation: If ablation deltas are tiny, the main gain may be from scheduler or layout, and paper should reduce triage claim strength.
- Table / figure target:
  - Figure 4: triage ablation bars
  - Table 3: deletion study
- Priority: MUST-RUN

### Block 4: Dynamic Scheduling Ablation

- Claim tested: C3
- Why this block exists: Proves dynamic io_uring scheduling is not just ordinary async I/O.
- Dataset / split / task: COCO100K, FlatStor, selected operating point.
- Compared systems:
  - Full dynamic scheduling
  - `--submit-online 0` vs `1`
  - `submit-batch` sweep: 4, 8, 16, 32
  - serial fetch after probe if supported; otherwise emulate with no overlap path and document limitation
  - `safein_all_threshold` sweep: small / default 256KB / effectively disabled
- Metrics:
  - avg/p95/p99 E2E latency
  - io_wait
  - uring_prep / uring_submit
  - submit calls
  - flush counts
  - bytes read and missing payload fetches
- Setup details:
  - Keep nprobe, topk, backend, cache protocol fixed.
  - Do not mix with CRC, RAIR, duplicate assignment, or cold-cache runs.
- Success criterion: Dynamic/default policy is near the Pareto front and lowers p95/p99 vs fixed or serial variants.
- Failure interpretation: If fixed `submit-batch` is equally good, simplify paper claim to “guarded selective fetch dominates; scheduler is implementation optimization.”
- Table / figure target:
  - Figure 5: p99 latency vs submit policy
  - Appendix table: scheduling parameter sweep
- Priority: MUST-RUN

### Block 5: Top-k and Payload Sensitivity

- Claim tested: C4
- Why this block exists: Demonstrates robustness when return set or payload cost grows.
- Dataset / split / task:
  - COCO top-k sensitivity
  - Deep1M_synth if generated; otherwise Deep8M_synth formal fallback or COCO synthetic payload padding
- Compared systems:
  - BoundFetch-Guarded
  - `IVF+PQ+FlatStor`
  - `IVF+RaBitQ+FlatStor`
- Metrics:
  - topk=10/20/50 quality-latency
  - bytes read
  - reranked candidates
  - payload reads
  - p99 latency
- Setup details:
  - `topk=20`: `candidate_budget=150` first pass, rerun 200 if needed.
  - `topk=50`: `candidate_budget=250`.
  - Synthetic payload sizes: formal baseline suggests `256B`, `4KB`, `64KB`; thesis minimum is `4KB`, strong version includes all three.
- Success criterion: BoundFetch-Guarded keeps lower read amplification as top-k/payload size increases.
- Failure interpretation: If top50 loses badly, keep top50 appendix and explain that large-k shifts bottleneck to required payload materialization.
- Table / figure target:
  - Appendix Figure A1: top-k sensitivity
  - Figure 6 or Appendix: payload size sensitivity
- Priority: NICE-TO-HAVE, but top20 COCO is SHOULD-RUN

## Run Order and Milestones

| Milestone | Goal | Runs | Decision Gate | Cost | Risk |
|-----------|------|------|---------------|------|------|
| M0 | Freeze assets and metric schema | Confirm COCO/MS MARCO embeddings, GT top100, qrels, FlatStor/Lance exports; verify `bench_e2e` output fields | Both datasets have base/query/GT/backend metadata | 0.5-1 day | MS MARCO qrels or Lance export missing |
| M1 | COCO sanity | One BoundFetch run at `nprobe=64`, one IVFPQ FlatStor run, one IVF-RaBitQ FlatStor run | Recall and E2E fields valid; SafeIn/SafeOut stats nonzero | 0.5 day | parameter mismatch or output schema missing |
| M2 | COCO main sweep | COCO all methods, nprobe sweep 16..512, topk=10 | Matched-quality operating point identifiable | 1-2 days | Lance runner blocks; keep FlatStor main and mark Lance risk |
| M3 | MS MARCO sanity/main | MS MARCO FlatStor first, then Lance; nprobe 16..1024 | At least FlatStor two baselines + BoundFetch complete | 2-4 days | MS MARCO build/GT larger than expected |
| M4 | Triage ablation | SafeOut off, SafeIn prefetch off, Uncertain eager payload, Stage2/boundary if supported | Full policy has clear read/rerank or latency advantage | 1-2 days | Some ablations require small code flags |
| M5 | Scheduling ablation | submit-online, submit-batch, serial/no-overlap, safein threshold | p95/p99 story clear | 1 day | serial path unavailable |
| M6 | Top-k/payload extras | COCO top20, optional top50; synthetic payload size | Robustness figure ready | 1-3 days | `recall@20/@50` output needs post-processing |
| M7 | Paper polish | Select tables, plot curves, write failure notes | Main paper tables covered | 1-2 days | inconsistent output paths |

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

### SHOULD-RUN: topk=20 Supplement

Run only after matched top10 points are known:

- COCO: BoundFetch + two FlatStor baselines at topk=20, candidate_budget=150/200.
- MS MARCO: same if COCO top20 shows meaningful trend.
- Lance top20 only if Lance top10 is stable.

### NICE-TO-HAVE: topk=50 and Synthetic Payload

- COCO topk=50 stress with candidate_budget=250.
- Deep1M_synth if generated; otherwise Deep8M_synth formal fallback.
- Payload sizes: minimum 4KB; full version 256B/4KB/64KB.

## Compute and Data Budget

- Total estimated GPU-hours: 0 if embeddings are already generated; otherwise MS MARCO embedding generation may dominate and should be handled outside this plan.
- CPU / I/O: main cost is index build, exact GT validation, and E2E sweeps over 6-7 nprobe values.
- Storage: expect formal baseline outputs under `/home/zcq/VDB/baselines/formal-study/outputs`; synthetic payload can require large temporary space.
- Human evaluation: none.
- Biggest bottleneck: MS MARCO readiness and Lance runner stability.

## Risks and Mitigations

- Risk: `msmarco_passage` qrels/ground truth incomplete.
  - Mitigation: run ANN `recall@10/@20` first; add `MRR@10/nDCG@10` once qrels path is validated.
- Risk: Lance four-combination E2E runner not stable.
  - Mitigation: main conclusion can use FlatStor four? Actually minimum keep BoundFetch + FlatStor baselines; Lance moves to appendix risk table if blocked.
- Risk: `recall@20/@50` not emitted by `bench_e2e`.
  - Mitigation: extend `recall_K` output or compute from per-query predictions; do not claim top20/top50 until verified.
- Risk: ablation flags do not exist for SafeOut/SafeIn/Uncertain variants.
  - Mitigation: first use available proxy parameters (`safein_all_threshold`, stage2 flags, `override-eps-ip`, `override-d-k`, submit flags); only add minimal flags if proxy cannot isolate the claim.
- Risk: Deep1M_synth not generated.
  - Mitigation: use formal `deep8m_synth` or COCO synthetic payload padding as fallback.
- Risk: cold/warm protocols get mixed.
  - Mitigation: thesis main table uses warm-serving only; cold/drop-cache is appendix only.

## Final Checklist

- [ ] COCO main table covered
- [ ] MS MARCO main table covered
- [ ] Four baseline combinations attempted and failures documented
- [ ] Matched-quality rule applied consistently
- [ ] Triage novelty isolated
- [ ] Dynamic scheduling isolated
- [ ] top20/top50 handled via benchmark extension or post-processing
- [ ] Nice-to-have runs separated from must-run runs
- [ ] No CRC/RAIR/duplicate clustering in main protocol
