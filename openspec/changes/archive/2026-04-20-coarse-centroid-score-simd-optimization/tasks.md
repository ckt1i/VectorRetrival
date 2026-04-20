## 1. Coarse Packed Layout

- [x] 1.1 在 `IvfIndex` 中为 coarse score 增加 packed centroid layout 持有结构，并区分 raw centroid 与 normalized centroid 两套视图。
- [x] 1.2 将 packed layout 的构建前移到 `IvfIndex::Open()` 或等价的 query 前准备阶段，确保 query 期间不再做临时 pack。
- [x] 1.3 为 `nlist` 非 block 整数倍、`dim` 非向量宽度整数倍的尾块定义补零或 mask 规则，并验证与原始 row-major 结果一致。

## 2. SIMD Coarse Score Kernel

- [x] 2.1 在 `include/vdb/simd/` 下新增 coarse `ip/cosine` centroid score SIMD kernel，采用多 centroid register-blocking 主实现。
- [x] 2.2 为 AVX2 / AVX-512 / scalar fallback 明确 dispatch 路径，并保证 `FindNearestClusters()` 可通过统一入口调用。
- [x] 2.3 在 `effective_metric=ip` 路径接入新的 SIMD coarse score kernel，替换当前逐 centroid 标量点积循环。

## 3. Query Path Integration

- [x] 3.1 保持 query normalize 和 top-n selection 逻辑不变，仅替换 centroid score 实现，确保本轮变更边界清晰。
- [x] 3.2 保持 `coarse_select_ms`、`coarse_score_ms`、`coarse_topn_ms` 的统计边界不变，并确认 `coarse_score_ms` 只覆盖新 kernel 的执行时间。
- [x] 3.3 为 coarse score 抽象统一接口，给后续 GEMV backend 接入保留扩展点，但本轮默认走 in-tree SIMD kernel。

## 4. Verification

- [x] 4.1 对比标量实现与 SIMD 实现的 coarse score 输出，验证 top-n cluster 顺序在容忍范围内一致。
- [x] 4.2 用固定参数跑一次同口径 E2E，记录 `coarse_select_ms`、`coarse_score_ms`、`coarse_topn_ms` 与总 query 时间变化。
- [x] 4.3 用同参数 query-only perf 验证热点是否从 coarse 标量点积迁移，并判断下一轮是否值得继续引入 GEMV backend。
