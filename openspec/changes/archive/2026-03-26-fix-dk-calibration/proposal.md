## Why

ConANN 的 `d_k` 校准当前仅从 database 向量自身采样 pseudo-query，假设 query 与 database 同分布。在跨模态检索场景（如 CLIP text→image），query 与 database 距离分布存在系统性偏移，导致 d_k 过小，所有向量被误判为 SafeOut（100%），ConANN 三分类完全失效。

## What Changes

- **新增 `CalibrateDistanceThreshold` 重载**：支持 query/database 分离的 d_k 校准，消除 self-distance 问题
- **`IvfBuilderConfig` 新增可选字段**：`calibration_queries` 和 `num_calibration_queries`，允许 build 时传入 query 样本
- **`IvfBuilder::CalibrateDk` 适配**：优先使用 query 样本校准 d_k，未提供时 fallback 到现有行为（database 自采样）
- **`bench_rabitq_accuracy` 修正**：Phase 3 使用 qry→img 校准 d_k
- **`bench_e2e` 适配**：如有 query 数据，传入 config 用于 d_k 校准

## Capabilities

### New Capabilities

_无新增 capability。_

### Modified Capabilities

- `per-cluster-epsilon`: d_k 校准源从 database-only 扩展为支持 query/database 分离

## Impact

- `include/vdb/index/conann.h` — 新增 CalibrateDistanceThreshold 重载
- `src/index/conann.cpp` — 实现新重载，旧签名改为 wrapper
- `include/vdb/index/ivf_builder.h` — IvfBuilderConfig 新增 calibration_queries 字段
- `src/index/ivf_builder.cpp` — CalibrateDk 适配新字段
- `tests/index/bench_rabitq_accuracy.cpp` — Phase 3 改用 qry 校准
- `tests/index/bench_e2e.cpp` — 传入 query 样本
- 现有 12+ 处 Build() 调用无需改动（config 默认 nullptr fallback）
