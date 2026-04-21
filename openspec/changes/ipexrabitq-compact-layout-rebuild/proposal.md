## Why

当前 `v10 packed_sign` 布局已经验证更适合单 candidate Stage2 扫描，而不适合 `batch=8` 的 true multi-candidate kernel。既然“batch 外壳 + 单 kernel”和“true batch kernel + 旧 layout”都已经退化，下一步必须把 Stage2 的磁盘格式和查询调度一起改成 batch-friendly 形态，才能真正压下 sign/code 带宽成本。

## What Changes

- 新增一条 `v11` Stage2 compact layout 路线，用 rebuild 生成面向 `batch=8` 的 blocked ExRaBitQ 存储格式。
- 将 Stage2 region 从当前按单向量顺序存放：
  - `code_abs[dim] + packed_sign[dim/8] + xipnorm`
  演进为按 `batch block × dim block` 存放的 compact blocked layout。
- 固定第一版 compact layout 参数：
  - `batch_size = 8`
  - `dim_block = 64`
  - `abs` 保持 `uint8`
  - `sign` 保持 packed bit，但按 block8 连续布局
  - `xipnorm` 按 block8 连续布局
- 在 cluster build / serialize 路径中直接生成 `v11 compact layout`，不在 query 时做在线 repack。
- 在 `ParsedCluster` / query path 中新增 batch-block 视图，替代当前 Stage2 仅按单 candidate 暴露 `ExRaBitQView` 的方式。
- 将 Stage2 调度从“收集 uncertain candidate list”改成“映射到 `block_id + lane_mask`，按 block 调 true batch kernel”。
- benchmark 和 perf 仍沿用当前正式低开销口径，但必须能区分：
  - rebuild 前 `v10 packed_sign`
  - rebuild 后 `v11 compact layout`
  的 Stage2 / E2E 收益。

## Capabilities

### New Capabilities
- `ipexrabitq-compact-layout`: 定义 `v11` Stage2 compact blocked 磁盘格式、batch block 视图和 rebuild 要求。
- `stage2-block-scheduling`: 定义 Stage2 从 uncertain candidate list 演进到 `block_id + lane_mask` 调度的执行边界和语义约束。

### Modified Capabilities
- `query-pipeline`: query 主路径需要新增 compact Stage2 view、block-aware Stage2 scheduling，以及与现有 funnel / top-k 语义对齐的约束。
- `e2e-benchmark`: benchmark 需要支持 `v10` 与 `v11` 口径对比，并明确输出 compact layout 重建后的 Stage2 / E2E 结果。

## Impact

- 主要影响 Stage2 相关的 cluster build、cluster store、parsed cluster、Stage2 query path、SIMD kernel 入口和 benchmark/perf 归因。
- 该 change 引入新的 rebuild-required 存储格式版本；旧 `v10 packed_sign` 仍保留兼容读取，但正式 compact 路线依赖新索引重建。
- 当前 change 不扩展 CLI 语义，不把 Stage1、coarse、submit 链路混入实现范围。
