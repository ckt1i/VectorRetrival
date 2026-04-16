## 1. Formal Study Workspace Bootstrap

- [x] 1.1 Create `/home/zcq/VDB/baselines/formal-study/` workspace directories for `manifests/`, `scripts/`, `outputs/`, and `trackers/`
- [x] 1.2 Instantiate `dataset_registry.csv`, `encoder_registry.csv`, and `baseline_registry.csv` from the formal baseline study templates
- [x] 1.3 Instantiate `RUN_STATUS.csv` and `FAILURES.md` from the formal study tracking templates
- [x] 1.4 Add shared path and run-id conventions so every later script uses the same raw/formatted/output roots

## 2. Primary Dataset Preparation Pipeline

- [x] 2.1 Implement `prepare_datasets/coco_100k_manifest.py` to validate local COCO assets and write cleaned payload plus split manifest
- [x] 2.2 Implement `prepare_datasets/msmarco_download.sh` and `prepare_datasets/msmarco_prepare.py` to download, clean, and split MS MARCO Passage
- [x] 2.3 Implement `prepare_datasets/deep8m_link_or_copy.sh` and `prepare_datasets/deep8m_generate_synth_payload.py` for Deep8M raw assets and deterministic payload tiers
- [x] 2.4 Implement `prepare_datasets/esci_download.sh` and `prepare_datasets/esci_prepare.py` to download, clean, and split Amazon ESCI
- [x] 2.5 Add dataset-level sanity checks that verify required raw files, cleaned outputs, and split manifests before a dataset is marked runnable

## 3. Embedding And Ground Truth Generation

- [x] 3.1 Implement or wire up embedding builders for COCO, MS MARCO, and Amazon ESCI using frozen encoder registry entries
- [x] 3.2 Implement `build_groundtruth/coco_100k_gt.py`, `msmarco_gt.py`, `esci_gt.py`, and `deep8m_convert_gt.py`
- [x] 3.3 Preserve task labels under the dataset `gt/` roots for labeled datasets such as `msmarco_passage` and `amazon_esci`
- [x] 3.4 Add validation that embedding metric, normalization, and generated `gt_top10.npy` / `gt_top20.npy` match the dataset registry contract
- [x] 3.5 Extend the ground-truth contract to generate and validate `gt_top100.npy` for every primary dataset so the `topk=100` main experiment is evaluable

## 4. Payload Backend Export

- [x] 4.1 Implement payload export scripts for COCO, MS MARCO, and Amazon ESCI to emit aligned `FlatStor`, `Lance`, and `Parquet` backends
- [x] 4.2 Implement Deep8M size-tiered backend export for `256B`, `4KB`, and `64KB` payload families
- [x] 4.3 Add schema-alignment checks so all exported backends preserve the same logical field set per dataset
- [x] 4.4 Record backend output size, row count, and path metadata back into the formal study manifests

## 5. Baseline System Integration

- [x] 5.1 Integrate `Faiss IVFPQ + rerank` under the shared vector-search runner contract with disk-backed rerank
- [x] 5.2 Keep `Faiss IVFPQR` only as a diagnostic runner and exclude it from the phase-1 mandatory comparison contract
- [x] 5.3 Integrate `Faiss IVFResidualQuantizer` or `IVFLocalSearchQuantizer` as the mandatory IVF-RQ baseline
- [x] 5.4 Run the DiskANN thin-wrapper smoke gate and mark it `active` or `blocked` in the baseline registry without blocking phase-1 closure
- [x] 5.5 Register `BoundFetch`, `ConANN + Faiss`, and `Extended-RaBitQ` as deferred or optional entries without implementing them in phase 1

## 6. Coupled E2E And Benchmark Output Contract

- [x] 6.1 Implement formal-study coupled E2E runners for the active non-BoundFetch baseline family without modifying `benchmarks/bench_e2e.cpp`
- [x] 6.2 Standardize per-run metric fields for vector-search, coupled E2E, payload microbench, and build/startup outputs
- [x] 6.3 Implement payload microbench runners that write only to `outputs/payload_microbench/` and never override formal E2E outputs
- [x] 6.4 Add dataset-level E2E summary generation at `outputs/e2e/{dataset}/e2e_system_summary.csv`

## 7. Result Aggregation And Tracking

- [x] 7.1 Update `RUN_STATUS.csv` automatically for every scheduled, running, completed, failed, and retried run
- [x] 7.2 Append structured failure records to `FAILURES.md` with dataset, system, config, and log path
- [x] 7.3 Implement formal-study aggregation for per-system summary CSVs, build/startup tables, and plot-ready comparison tables
- [x] 7.4 Add controlled backfill from `/home/zcq/VDB/baselines/formal-study/outputs/` into `/home/zcq/VDB/baselines/results/` and the associated analysis documents

## 8. Batch Execution And Validation

- [x] 8.1 Run the first batch on `COCO 100K` for the mandatory non-BoundFetch baseline suite and at least one primary payload backend
- [ ] 8.2 Prepare and validate `MS MARCO Passage` for baseline-ready search, coupled E2E, and label-aware outputs
- [x] 8.3 Prepare and validate `Deep8M-synth` across the three payload-size tiers and storage/backend comparisons
- [ ] 8.4 Prepare and validate `Amazon ESCI` for baseline-ready search, coupled E2E, and label-aware outputs
- [x] 8.5 Materialize appendix dataset manifests and sanity-run hooks for `LAION subset`, `Clotho`, and `MSR-VTT`, gated behind the primary suite
- [x] 8.6 Freeze the main-experiment search controls with `topk ∈ {10,50,100}`, `candidate_budget ∈ {100,250,500}` mapped to `topk`, `FlatStor` as the primary backend, and dataset-specific `nlist` values (`COCO=2048`, `Deep8M=12800`, `MS MARCO=16384`, `Amazon ESCI=8192` unless cardinality review changes it)
- [ ] 8.7 Run the main experiment suite for each `dataset × method × topk` by varying only `nprobe` on the frozen `nlist` values, and vary only `L_search` for DiskANN if it remains active
- [ ] 8.8 Aggregate the main experiment suite into `topk=10 / 50 / 100` summaries and plots, and use the top-10 results as the only source for backend operating-point selection
- [ ] 8.9 Select the extended-experiment backend operating points by targeting `recall@10 ≈ 0.80`, `0.90`, and `0.95` for each `dataset × method`, with explicit `unreached` or `best-effort` handling when needed
- [ ] 8.10 Run `FlatStor`, `Lance`, and `Parquet` only on the selected top-10 recall-matched operating points instead of replaying a full parameter grid
- [ ] 8.11 Aggregate the extended backend experiment into payload/backend comparison summaries and plot-ready CSVs that remain distinct from the main experiment suite
- [ ] 8.12 Gate appendix-dataset execution on successful completion of both the main experiment suite and the top-10 backend extension suite
