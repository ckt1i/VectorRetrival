## 1. Restore the thesis main-sweep workspace contract

> Note 2026-05-03: the latest cleanup invalidated only the previous
> BoundFetch measurement runs (`T002`, `T005`, `T010`, `T011`) after a
> BoundFetch-specific bench path issue was identified. Baseline tasks and
> canonical/reuse gates remain retained unless a later spec change says
> otherwise.

- [x] 1.1 Recreate the `thesis-minimal-main-sweep` change artifacts and confirm they replace the accidentally deleted draft.
- [x] 1.2 Align the change scope with `refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_PLAN.md` and `EXPERIMENT_TRACKER.md` for `T000-T015` only.
- [x] 1.3 Identify the current runnable formal-study entrypoints and resolve the gap between `/home/zcq/VDB/baselines/formal-study/README.md` and the actual `legacy/scripts/` layout.
- [x] 1.4 Freeze the thesis output roots, tracker fields, and task-to-run mapping needed for asset checks, index builds, reuse validation, sanity runs, and full sweeps.

## 2. Complete thesis asset gates `T000-T001`

- [x] 2.1 Validate `coco_100k` base/query embeddings, GT top100, and backend exports required by `T000`.
- [x] 2.2 Validate `msmarco_passage` base/query embeddings, GT top100, and backend exports required by `T001`.
- [x] 2.3 Check `msmarco_passage` qrels availability and record whether `MRR@10/nDCG@10` are ready or delayed.
- [x] 2.4 Record both asset checks in the thesis tracker with explicit pass/block status and artifact paths.

## 3. Add canonical index build and reuse-validation support

- [x] 3.1 Define the canonical artifact directory and metadata contract for thesis index builds under `outputs/index_build/`.
- [x] 3.2 Add or restore the `bench_e2e` invocation path that builds `coco_100k` canonical artifacts with `--coarse-builder faiss_kmeans`.
- [x] 3.3 Add or restore the `bench_e2e` invocation path that builds `msmarco_passage` canonical artifacts with `--coarse-builder hierarchical_superkmeans`.
- [x] 3.4 Add explicit external-artifact consumption inputs for BoundFetch-Guarded so thesis runs can consume a prebuilt canonical index directory.
- [x] 3.5 Add explicit external-artifact consumption or no-retrain import inputs for `faiss_ivfpq_refine`.
- [x] 3.6 Add explicit external-artifact consumption or no-retrain import inputs for `ivf_rabitq_rerank`.
- [x] 3.7 Ensure thesis benchmark and runner outputs record canonical artifact provenance instead of silently rebuilding from raw vectors.
- [x] 3.8 Add the thesis warmup-then-measurement protocol to the full-sweep runner so every formal operating point has a non-authoritative warmup followed by a strict-recall measurement.
- [x] 3.9 Freeze BoundFetch-Guarded mainline runner parameters as `crc=1`, `early-stop=0`, `bits=4`, and `skip_gt=0` for measurement, while keeping non-mainline modes out of thesis summaries.

## 4. Run COCO canonical-artifact validation and sanity gates

- [x] 4.1 Build the canonical `coco_100k` artifact after `T000` using `faiss_kmeans` and record its provenance.
- [x] 4.2 Validate that BoundFetch-Guarded can execute on `coco_100k` from the canonical artifact path.
- [x] 4.3 Validate that `IVF+PQ` can execute on `coco_100k` from the canonical artifact path or deterministic no-retrain import path.
- [x] 4.4 Validate that `IVF+RaBitQ` can execute on `coco_100k` from the canonical artifact path or deterministic no-retrain import path.
- [x] 4.5 Run `T002` COCO BoundFetch-Guarded sanity and verify recall, E2E, p99, and triage fields.
- [x] 4.6 Run `T003` COCO `IVF+PQ+FlatStor` sanity and verify recall, E2E, and p99 fields.
- [x] 4.7 Run `T004` COCO `IVF+RaBitQ+FlatStor` sanity and verify recall, E2E, and p99 fields.

## 5. Execute the COCO full thesis main sweep `T005-T009`

- [x] 5.1 Run `T005` COCO BoundFetch-Guarded full `nprobe` sweep with the frozen warmup-then-measurement protocol and `crc=1, early-stop=0`.
- [x] 5.2 Run `T006` COCO `IVF+PQ+FlatStor` full `nprobe` sweep.
- [x] 5.3 Run `T007` COCO `IVF+RaBitQ+FlatStor` full `nprobe` sweep.
- [x] 5.4 Run `T008` COCO `IVF+PQ+Lance` full `nprobe` sweep or record an explicit blocked rationale.
- [x] 5.5 Run `T009` COCO `IVF+RaBitQ+Lance` full `nprobe` sweep or record an explicit blocked rationale.
- [x] 5.6 Aggregate the COCO vector-search and E2E outputs into a dataset-level thesis summary.

## 6. Run MS MARCO canonical-artifact validation and sanity gate

- [x] 6.1 Build the canonical `msmarco_passage` artifact after `T001` using `hierarchical_superkmeans` and record its provenance.
- [x] 6.2 Validate that BoundFetch-Guarded can execute on `msmarco_passage` from the canonical artifact path.
- [x] 6.3 Validate that `IVF+PQ` can execute on `msmarco_passage` from the canonical artifact path or deterministic no-retrain import path.
- [x] 6.4 Validate that `IVF+RaBitQ` can execute on `msmarco_passage` from the canonical artifact path or deterministic no-retrain import path.
- [x] 6.5 Run `T010` MS MARCO BoundFetch-Guarded sanity and verify recall, E2E, p99, and triage fields.

## 7. Execute the MS MARCO full thesis main sweep `T011-T015`

- [x] 7.1 Run `T011` MS MARCO BoundFetch-Guarded full `nprobe` sweep with the frozen warmup-then-measurement protocol and `crc=1, early-stop=0`.
- [x] 7.2 Run `T012` MS MARCO `IVF+PQ+FlatStor` full `nprobe` sweep.
- [x] 7.3 Run `T013` MS MARCO `IVF+RaBitQ+FlatStor` full `nprobe` sweep.
- [x] 7.4 Run `T014` MS MARCO `IVF+PQ+Lance` full `nprobe` sweep or record an explicit blocked rationale.
- [x] 7.5 Run `T015` MS MARCO `IVF+RaBitQ+Lance` full `nprobe` sweep or record an explicit blocked rationale.
- [x] 7.6 Aggregate the MS MARCO vector-search and E2E outputs into a dataset-level thesis summary, marking delayed qrels metrics explicitly if needed.

## 8. Close out thesis main tracking and summary generation

- [x] 8.1 Update `RUN_STATUS.csv` and `FAILURES.md` so every thesis subtask is auditable by run id, phase, dataset, system, and artifact provenance.
- [x] 8.2 Emit dataset-level matched-quality selection inputs for COCO and MS MARCO without mixing them with appendix or deferred experiments.
- [ ] 8.3 Record any Lance or qrels blockers as explicit thesis main-sweep risks rather than silently dropping those branches.
- [x] 8.4 Verify dataset summaries select only strict-recall measurement runs, not warmup, smoke, debug, `skip_gt=1`, or non-mainline BoundFetch runs.
