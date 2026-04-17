# Formal Baseline Study

Date: 2026-04-16

This directory is the standalone workspace for the new formal baseline study.
It is intentionally separated from the active BoundFetch optimization logs so
that baseline planning, dataset expansion, and experiment bookkeeping can evolve
without rewriting the main method documents.

Files:

- `01_BASELINE_STRATEGY_CN.md`
  - Defines the paper-facing comparison strategy.
  - Maps BoundFetch's current contributions to the baseline layers that must be
    covered.
- `02_DATASET_AND_EMBEDDING_PLAN_CN.md`
  - Defines datasets, payload forms, embedding generation, and synthetic data
    policy.
- `03_BASELINE_METHODS_CN.md`
  - Defines the vector-search baseline families, the fixed 4-bit IVF baseline
    policy, storage backend semantics, and what is considered main-table vs
    reference-only.
- `04_EXECUTION_PLAYBOOK_CN.md`
  - Defines the execution order, output files, and experiment recording rules.
- `05_RUN_STATUS_TEMPLATE.csv`
  - Template for run-level status tracking.
- `06_DATASET_REGISTRY_TEMPLATE.csv`
  - Template for dataset and embedding bookkeeping.
- `07_ENCODER_REGISTRY_TEMPLATE.csv`
  - Template for encoder/model bookkeeping.
- `08_DOWNLOAD_AND_PREP_CHECKLIST_CN.md`
  - Dataset download, preprocessing, embedding, and payload-prep checklist.
- `09_BASELINE_REGISTRY_TEMPLATE.csv`
  - Template for baseline implementation bookkeeping.
- `10_DATA_PREP_SCRIPT_CHECKLIST_CN.md`
  - Script-level checklist for dataset preparation.

Scope:

- Main goal:
  - Build a defensible formal baseline suite for BoundFetch as an integrated
    vector-search-plus-payload system.
  - Freeze the current formal comparison around `IVF+PQ`, `IVF+RaBitQ`, and
    `DiskANN`, with 4-bit as the official IVF-side quantization budget.
- Not in scope:
  - Replacing the active BoundFetch optimization plan.
  - Expanding into a full `search x storage x dataset` Cartesian matrix.
  - Treating DiskANN as the only primary benchmark target.

Path conventions:

- Raw datasets and raw embeddings:
  - `/home/zcq/VDB/data`
- Cleaned canonical tables, split manifests, ground truth, and
  baseline/backend formatted storage data:
  - `/home/zcq/VDB/baselines/data`
- Baseline experiment outputs, plots, and trackers:
  - `/home/zcq/VDB/baselines`
- Manually downloaded or compiled third-party libraries:
  - `/home/zcq/VDB/third_party`

Recommended usage:

1. Freeze the dataset and encoder choices in `02_*`.
2. Freeze the baseline set in `03_*`.
3. Run experiments strictly following `04_*`.
4. Initialize tracking from `05_*` and `06_*`.
5. Freeze encoder choices in `07_*`.
6. Follow the execution checklist in `08_*`.
7. Write all new figures, CSVs, and tracker notes against this folder first.
