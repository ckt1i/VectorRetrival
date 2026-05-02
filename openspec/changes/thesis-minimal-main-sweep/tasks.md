## 1. Restore the thesis main-sweep workspace contract

- [ ] 1.1 Recreate the `thesis-minimal-main-sweep` change artifacts and confirm they replace the accidentally deleted draft.
- [ ] 1.2 Align the change scope with `refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_PLAN.md` and `EXPERIMENT_TRACKER.md` for `T000-T015` only.
- [ ] 1.3 Identify the current runnable formal-study entrypoints and resolve the gap between `/home/zcq/VDB/baselines/formal-study/README.md` and the actual `legacy/scripts/` layout.
- [ ] 1.4 Freeze the thesis output roots, tracker fields, and task-to-run mapping needed for asset checks, index builds, reuse validation, sanity runs, and full sweeps.

## 2. Complete thesis asset gates `T000-T001`

- [ ] 2.1 Validate `coco_100k` base/query embeddings, GT top100, and backend exports required by `T000`.
- [ ] 2.2 Validate `msmarco_passage` base/query embeddings, GT top100, and backend exports required by `T001`.
- [ ] 2.3 Check `msmarco_passage` qrels availability and record whether `MRR@10/nDCG@10` are ready or delayed.
- [ ] 2.4 Record both asset checks in the thesis tracker with explicit pass/block status and artifact paths.

## 3. Add canonical index build and reuse-validation support

- [ ] 3.1 Define the canonical artifact directory and metadata contract for thesis index builds under `outputs/index_build/`.
- [ ] 3.2 Add or restore the `bench_e2e` invocation path that builds `coco_100k` canonical artifacts with `--coarse-builder faiss_kmeans`.
- [ ] 3.3 Add or restore the `bench_e2e` invocation path that builds `msmarco_passage` canonical artifacts with `--coarse-builder hierarchical_superkmeans`.
- [ ] 3.4 Add explicit external-artifact consumption inputs for BoundFetch-Guarded so thesis runs can consume a prebuilt canonical index directory.
- [ ] 3.5 Add explicit external-artifact consumption or no-retrain import inputs for `faiss_ivfpq_refine`.
- [ ] 3.6 Add explicit external-artifact consumption or no-retrain import inputs for `ivf_rabitq_rerank`.
- [ ] 3.7 Ensure thesis benchmark and runner outputs record canonical artifact provenance instead of silently rebuilding from raw vectors.

## 4. Run COCO canonical-artifact validation and sanity gates

- [ ] 4.1 Build the canonical `coco_100k` artifact after `T000` using `faiss_kmeans` and record its provenance.
- [ ] 4.2 Validate that BoundFetch-Guarded can execute on `coco_100k` from the canonical artifact path.
- [ ] 4.3 Validate that `IVF+PQ` can execute on `coco_100k` from the canonical artifact path or deterministic no-retrain import path.
- [ ] 4.4 Validate that `IVF+RaBitQ` can execute on `coco_100k` from the canonical artifact path or deterministic no-retrain import path.
- [ ] 4.5 Run `T002` COCO BoundFetch-Guarded sanity and verify recall, E2E, p99, and triage fields.
- [ ] 4.6 Run `T003` COCO `IVF+PQ+FlatStor` sanity and verify recall, E2E, and p99 fields.
- [ ] 4.7 Run `T004` COCO `IVF+RaBitQ+FlatStor` sanity and verify recall, E2E, and p99 fields.

## 5. Execute the COCO full thesis main sweep `T005-T009`

- [ ] 5.1 Run `T005` COCO BoundFetch-Guarded full `nprobe` sweep with the frozen warm protocol.
- [ ] 5.2 Run `T006` COCO `IVF+PQ+FlatStor` full `nprobe` sweep.
- [ ] 5.3 Run `T007` COCO `IVF+RaBitQ+FlatStor` full `nprobe` sweep.
- [ ] 5.4 Run `T008` COCO `IVF+PQ+Lance` full `nprobe` sweep or record an explicit blocked rationale.
- [ ] 5.5 Run `T009` COCO `IVF+RaBitQ+Lance` full `nprobe` sweep or record an explicit blocked rationale.
- [ ] 5.6 Aggregate the COCO vector-search and E2E outputs into a dataset-level thesis summary.

## 6. Run MS MARCO canonical-artifact validation and sanity gate

- [ ] 6.1 Build the canonical `msmarco_passage` artifact after `T001` using `hierarchical_superkmeans` and record its provenance.
- [ ] 6.2 Validate that BoundFetch-Guarded can execute on `msmarco_passage` from the canonical artifact path.
- [ ] 6.3 Validate that `IVF+PQ` can execute on `msmarco_passage` from the canonical artifact path or deterministic no-retrain import path.
- [ ] 6.4 Validate that `IVF+RaBitQ` can execute on `msmarco_passage` from the canonical artifact path or deterministic no-retrain import path.
- [ ] 6.5 Run `T010` MS MARCO BoundFetch-Guarded sanity and verify recall, E2E, p99, and triage fields.

## 7. Execute the MS MARCO full thesis main sweep `T011-T015`

- [ ] 7.1 Run `T011` MS MARCO BoundFetch-Guarded full `nprobe` sweep with the frozen warm protocol.
- [ ] 7.2 Run `T012` MS MARCO `IVF+PQ+FlatStor` full `nprobe` sweep.
- [ ] 7.3 Run `T013` MS MARCO `IVF+RaBitQ+FlatStor` full `nprobe` sweep.
- [ ] 7.4 Run `T014` MS MARCO `IVF+PQ+Lance` full `nprobe` sweep or record an explicit blocked rationale.
- [ ] 7.5 Run `T015` MS MARCO `IVF+RaBitQ+Lance` full `nprobe` sweep or record an explicit blocked rationale.
- [ ] 7.6 Aggregate the MS MARCO vector-search and E2E outputs into a dataset-level thesis summary, marking delayed qrels metrics explicitly if needed.

## 8. Close out thesis main tracking and summary generation

- [ ] 8.1 Update `RUN_STATUS.csv` and `FAILURES.md` so every thesis subtask is auditable by run id, phase, dataset, system, and artifact provenance.
- [ ] 8.2 Emit dataset-level matched-quality selection inputs for COCO and MS MARCO without mixing them with appendix or deferred experiments.
- [ ] 8.3 Record any Lance or qrels blockers as explicit thesis main-sweep risks rather than silently dropping those branches.
