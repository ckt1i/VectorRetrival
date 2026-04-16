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
  - Defines the vector-search baseline families, storage backends, and what is
    considered main-table vs reference-only.
- `04_EXECUTION_PLAYBOOK_CN.md`
  - Defines the execution order, output files, and experiment recording rules.
- `05_RUN_STATUS_TEMPLATE.csv`
  - Template for run-level status tracking.
- `06_DATASET_REGISTRY_TEMPLATE.csv`
  - Template for dataset and embedding bookkeeping.

Scope:

- Main goal:
  - Build a defensible formal baseline suite for BoundFetch as an integrated
    vector-search-plus-payload system.
- Not in scope:
  - Replacing the active BoundFetch optimization plan.
  - Expanding into a full `search x storage x dataset` Cartesian matrix.
  - Treating DiskANN as the only primary benchmark target.

Recommended usage:

1. Freeze the dataset and encoder choices in `02_*`.
2. Freeze the baseline set in `03_*`.
3. Run experiments strictly following `04_*`.
4. Initialize tracking from `05_*` and `06_*`.
5. Write all new figures, CSVs, and tracker notes against this folder first.
