## Why

当前 `coco_100k / nlist=2048 / nprobe=64 / full_preload / use_resident_clusters=1` 的查询路径中，`coarse_score_ms` 已经占据了 coarse 阶段的绝大部分时间，并且当前 `cosine -> effective ip` 的 centroid score 仍是逐 centroid、逐维度的标量点积实现。继续优化 probe、rerank 或 resident wrapper 而不处理 coarse centroid score，会让 query 主路径很快碰到新的固定上限。

现在推进这项变更，是因为最新 profiling 已经明确表明 `coarse_topn_ms` 不是主要问题，真正的热点是 centroid score kernel 本身。此时优先把 coarse score 改成面向多 centroid 的 SIMD register-blocking 路径，收益最直接、边界也最清晰。

## What Changes

- 为 `FindNearestClusters()` 的 `effective_metric=ip` coarse score 路径引入多 centroid 并行展开的 SIMD kernel，替换当前逐 centroid 标量点积循环。
- 为 coarse score 新增专用的 packed centroid layout，使 query 阶段直接消费面向 SIMD/register blocking 的内存布局，而不是在 query 期间临时重排。
- 将 `centroids_` / `normalized_centroids_` 的 coarse 专用布局准备前移到索引打开或查询前准备阶段，避免 query 开始后再做布局化转换。
- 保留现有 query normalize 和 top-n selection 语义，本轮不重写这两部分逻辑，仅要求 coarse score 优化后继续稳定输出 `coarse_select_ms`、`coarse_score_ms`、`coarse_topn_ms`。
- 在设计中明确 SIMD 与 GEMV 的关系，并为后续引入 GEMV backend 预留接口；但本轮实现目标固定为 in-tree SIMD register-blocking coarse kernel，不要求同时切换到 BLAS/GEMV 主路径。

## Capabilities

### New Capabilities
- `coarse-centroid-score-kernel`: 定义 coarse centroid score 在 `effective_metric=ip` 路径下的 SIMD register-blocking 计算要求，以及 coarse packed centroid layout 的准备与使用边界。

### Modified Capabilities
- `query-pipeline`: 修改 coarse cluster selection 的执行要求，允许 query path 使用预先准备好的 packed centroid layout 与 SIMD centroid score kernel。
- `e2e-benchmark`: 修改 benchmark 输出要求，要求 coarse score 优化后继续稳定导出 coarse 细分字段，并能支撑优化前后对照。

## Impact

- 受影响代码主要在 [ivf_index.h](/home/zcq/VDB/VectorRetrival/include/vdb/index/ivf_index.h)、[ivf_index.cpp](/home/zcq/VDB/VectorRetrival/src/index/ivf_index.cpp) 以及 `include/vdb/simd/` 下新增 coarse score SIMD kernel 的位置。
- `IvfIndex::Open()` 将承担 coarse packed layout 的准备责任，索引内存占用会增加一份 coarse 专用布局的常驻副本。
- benchmark 不新增主开关，但 coarse 相关统计字段必须继续保留，便于后续用同参数 E2E / perf 对比收益。
