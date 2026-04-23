## Why

在当前 `full_preload + resident=1 + crc=1 + nprobe=256` 的主工作点上，query 路径的最大 CPU 成本已经收敛到 per-cluster FastScan prepare。最新细粒度 benchmark 显示 `probe_prepare_ms` 明显高于 `probe_stage1_ms` 和 `probe_submit_ms`，且 prepare 内部主要耗时集中在 fused FastScan 的 `quantize` 与 `lut_build`，说明现有 hot path 仍有较多中间态 materialization、重复 scratch 管理和未完全融合的 LUT 生成开销。

现在需要把这条 resident query prepare 路径进一步收敛成真正的 hot path：减少每 cluster 固定税，保留现有统计口径，并让 fused FastScan prepare 更接近“单遍流式生成”而不是“逻辑上融合、实现上仍分段”。由于后续还要在 `96 / 512 / 1024 / 3072` 等不同维度和非 `2^n` 维度的数据集上验证，这轮 change 的主目标应是拿到跨维度稳定收益，而不是围绕某一个固定维度做专用优化。

## What Changes

- 强化 resident FastScan prepare hot path，优先优化 `PrepareQueryRotatedInto()` 的 scratch 复用、固定容量管理和热路径状态维护成本。
- 重写 fused FastScan prepare 的核心生成路径，使 `quantize + lut_build` 更接近单遍流式生成，减少 `q_chunk` 一类中间落地与 group 级小粒度循环开销。
- 将本轮优化的重心限定为维度无关的通用 hot-path 改进，并仅将 specialization 视为后续可选增强，而不是本 change 的核心承诺。
- 保持并细化现有 benchmark 可观测性，使 `probe_prepare_ms` 及其子项仍能稳定归因到 `subtract / normalize / quantize / lut_build`。
- 将 Stage1 后续优化范围收敛到 `EstimateDistanceFastScan` 主体，明确 mask / iterate / classify 已不是优先热点。

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `fastscan-lut-fusion`: 收紧 fused FastScan prepare 的要求，要求实现进一步减少中间量化缓冲往返，并优先保证对不同维度工作点都成立的流式生成优化边界。
- `fastscan-stage1-optimization`: 补充 query prepare 细粒度可观测性与优化边界，明确 prepare 优化重点应落在 `quantize`、`lut_build` 与 `EstimateDistanceFastScan` 主循环，而不是重新隐藏到黑盒总量里。
- `resident-query-hotpath`: 强化 resident hot path 对固定容量 scratch / wrapper 复用的要求，避免在每个 cluster prepare 中重复执行可前移的容器状态维护。

## Impact

- Affected code:
  - `src/rabitq/rabitq_estimator.cpp`
  - `src/simd/fastscan.cpp`
  - `include/vdb/rabitq/rabitq_estimator.h`
  - benchmark timing/reporting paths in `benchmarks/bench_e2e.cpp`
- Affected systems:
  - resident full-preload query path
  - FastScan prepare / Stage1 estimation path
  - benchmark observability for query substage attribution
- No intended API or recall-semantic changes; this is a hot-path performance optimization change.
