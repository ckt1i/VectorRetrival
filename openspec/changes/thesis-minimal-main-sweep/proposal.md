## Why

`formal-baseline-study-execution` 定义的是更大范围的 formal baseline study，但当前本科论文主线已经收缩为 `coco_100k` 与 `msmarco_passage` 两个真实 workload 上的最小完整主实验闭环。现在需要一条新的、可执行的 OpenSpec change，把 `refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_TRACKER.md` 中 `T000-T015` 的资产检查、索引构建、索引复用验证、sanity run 和完整 main sweep 冻结成单独合同，避免继续被旧 formal-study 的大矩阵、appendix 数据集和历史 run 状态干扰。

## What Changes

- 新增一套 thesis-minimal main sweep 执行合同，只覆盖 `T000-T015`：
  - `coco_100k` 与 `msmarco_passage` 的资产检查
  - 主方法 `BoundFetch-Guarded`
  - 四个 baseline 组合：
    - `IVF+PQ+FlatStor`
    - `IVF+RaBitQ+FlatStor`
    - `IVF+PQ+Lance`
    - `IVF+RaBitQ+Lance`
- 冻结主实验运行矩阵：
  - `queries=1000`
  - `topk=10`
  - baseline `candidate_budget=100`
  - `coco_100k`: `nlist=2048`, `nprobe=16/32/64/128/256/512`
  - `msmarco_passage`: `nlist=16384`, `nprobe=16/32/64/128/256/512/1024`
- 新增“索引先行”要求：
  - 在完成 `T000-T001` 资产检查后，先用 `bench_e2e` 的 `faiss_kmeans` coarse-builder 路径为 `coco_100k` 生成 canonical 聚类与索引工件
  - 在 `coco_100k` 上验证 BoundFetch-Guarded、`IVF+PQ` 和 `IVF+RaBitQ` 都支持直接复用这些已生成的索引工件运行实验，或者仅通过无重训的导入步骤消费这些工件，而不是各自重新对原始向量做聚类/建索引
  - 再用 `bench_e2e` 的 `hierarchical_superkmeans` coarse-builder 路径为 `msmarco_passage` 生成 canonical 聚类与索引工件
  - 在 `msmarco_passage` 上验证主方法与 baseline 同样支持直接复用既有索引工件，或者仅通过无重训导入步骤消费这些工件
- 新增 dataset-level sanity gate：
  - `coco_100k` 先完成 `BoundFetch-Guarded`、`IVF+PQ+FlatStor`、`IVF+RaBitQ+FlatStor` 单点 sanity
  - `msmarco_passage` 先完成 `BoundFetch-Guarded` 单点 sanity，再进入 full sweep
- 复用现有 formal-study 的输出目录、per-run metrics、tracker 和 summary 口径，不另起一套 thesis-only 输出协议
- 明确这版 change 不覆盖：
  - `deep1m_synth` / `deep8m_synth`
  - `topk=20/50/100` 补充实验
  - triage ablation / scheduling ablation；这些由 `thesis-minimal-ablation-study` 单独承接
  - Amazon ESCI / appendix 数据集
  - DiskANN / IVFRQ / IVFPQR 主表比较
- 明确 `tasks.md` 必须进一步细分子任务，不允许只保留粗粒度 milestone

## Capabilities

### New Capabilities
- `thesis-minimal-main-execution`: 定义 `coco_100k` 与 `msmarco_passage` 上 `T000-T015` 的资产检查、索引构建、索引复用验证、sanity gate 和 full sweep 运行合同
- `thesis-minimal-index-reuse`: 定义 canonical 聚类/索引工件的生成路径、命名、复用要求，以及主方法与 baseline 对“使用既有索引文件”的兼容性验证
- `thesis-minimal-main-tracking`: 定义 thesis 最小主实验的 `RUN_STATUS.csv`、`FAILURES.md`、per-run metrics、dataset summary 和 matched-quality 主结果选择规则

### Modified Capabilities
- `e2e-benchmark`: 补充 thesis 最小主实验对 canonical 索引输入、复用现有聚类/索引工件、以及 sanity/full-sweep 输出一致性的要求
- `benchmark-infra`: 补充用 `bench_e2e` 生成 `faiss` 与 `hierarchical_superkmeans` canonical 索引工件并将其交给外部 runner 复用的实验合同

## Impact

- Affected docs:
  - `/home/zcq/VDB/VectorRetrival/refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_PLAN.md`
  - `/home/zcq/VDB/VectorRetrival/refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_TRACKER.md`
  - `/home/zcq/VDB/VectorRetrival/openspec/changes/formal-baseline-study-execution/*` (reference only; not the new source of truth)
- Affected code and scripts:
  - `/home/zcq/VDB/VectorRetrival/benchmarks/bench_e2e.cpp`
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/run_vector_search/*`
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/run_e2e_coupled/*`
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/aggregate/*`
- Affected manifests and trackers:
  - `/home/zcq/VDB/baselines/formal-study/manifests/*`
  - `/home/zcq/VDB/baselines/formal-study/trackers/*`
  - `/home/zcq/VDB/baselines/formal-study/outputs/*`
- Affected datasets and generated assets:
  - `/home/zcq/VDB/data/formal_baselines/coco_100k`
  - `/home/zcq/VDB/data/formal_baselines/msmarco_passage`
  - `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k`
  - `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage`
