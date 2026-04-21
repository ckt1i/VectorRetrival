## Why

当前 BoundFetch 的 `full_preload` 已经能够把 `.clu` 文件整体读入内存，但查询热路径里仍然残留 resident cluster 到 `ParsedCluster` 的重建、地址向量复制，以及首次查询触发 preload 的启动期开销，导致 warm-serving 对比仍然把一部分本应属于启动期的成本记进了查询延迟。与此同时，现有 IVF-RaBitQ baseline 默认把 compressed index 作为常驻内存结构直接搜索，因此当前对比口径并不公平，也无法回答“如果 compressed plane 全部常驻，BoundFetch 的 steady-state 搜索速度还能提升多少”。

## What Changes

- 新增一条 `resident-cluster-warm-path` 能力，将 `.clu` 的全部 cluster block、量化向量视图和原始 block 地址在查询开始前一次性 preload 并解析为全局常驻的 probe-ready 结构。
- 修改现有 `query-pipeline`，使 `full_preload` 模式下的查询热路径不再执行任何 `.clu` 读取、cluster parse、resident-to-ParsedCluster 重建或地址复制，而是直接在 resident cluster view 上完成量化向量搜索。
- 修改现有 `e2e-benchmark`，新增 preload 前后对照实验协议，要求在同一组 warm 参数下分别测量：
  - 现有 `full_preload` 路径
  - 新的 resident warm path
  并输出搜索速度提升、preload 时间和额外常驻内存占用。
- 为 benchmark 输出补齐 resident 模式显式字段，至少包括：
  - `clu_preload_time_ms`
  - `clu_preload_bytes`
  - `resident_cluster_mem_bytes`
  - `query_uses_resident_clusters`
- 将本次 change 的验证目标固定为 `coco_100k` warm steady-state，对同一组参数做 before/after 对照，判断 compressed-plane resident 化是否能显著降低查询期 CPU/调度开销。

## Capabilities

### New Capabilities
- `resident-cluster-warm-path`: 定义 `.clu` 全量 preload 后的常驻 cluster 结构、查询期零 `.clu` 准备动作以及 resident probe 协议

### Modified Capabilities
- `query-pipeline`: 调整 `full_preload` 模式的查询语义，使其从“查询期懒触发 preload + per-query resident materialization”改为“启动期 preload + 查询期直接 resident probe”
- `e2e-benchmark`: 增加 resident warm 模式的前后对照实验要求、preload 指标输出和性能提升验收门槛

## Impact

- Affected code:
  - `src/storage/cluster_store.cpp`
  - `include/vdb/storage/cluster_store.h`
  - `include/vdb/storage/segment.h`
  - `src/query/overlap_scheduler.cpp`
  - `benchmarks/bench_e2e.cpp`
- Affected runtime behavior:
  - `full_preload` 将被进一步收束为真正的 resident compressed-plane warm path
  - 查询开始前需要显式完成 `.clu` preload / parse
  - 查询期的量化向量搜索将完全在内存中完成
- Affected benchmark outputs:
  - `test/.../results.json`
  - `baselines/results/e2e_comparison_warm.csv`
  - `baselines/results/analysis.md`
- Affected evaluation protocol:
  - 固定以 `coco_100k`、warm steady-state、相同参数的 before/after 对照来验证 resident path 的收益
  - preload 成本单独记录，不混入 steady-state `e2e_ms`
