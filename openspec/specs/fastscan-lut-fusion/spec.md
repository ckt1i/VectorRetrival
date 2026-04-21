# Spec: FastScan LUT Fusion

## Overview

FastScan LUT fusion optimization for query preparation, enabling fused quantize and LUT build operations.

## ADDED Requirements (from quantize-lut-fusion-simd-optimization)

### Requirement: FastScan prepare SHALL support fused quantize and LUT build
系统 SHALL 支持一条 fused `quantize + lut_build` prepare 路径。该路径 MUST 允许在 block 或寄存器粒度上完成量化、16-entry group LUT 生成、lo/hi byte plane 生成以及最终 packed LUT 写出，而不要求先完整 materialize 再重新读取整条 `quant_query` 缓冲。

#### Scenario: Fused path does not require full intermediate quant buffer round-trip
- **WHEN** 系统启用 fused FastScan prepare 路径
- **THEN** 实现 MUST 不依赖"完整写出 `quant_query[dim]` 再完整读回"的两遍主路径
- **AND** 允许仅存在局部 scratch 或寄存器级中间态

### Requirement: Fused prepare SHALL preserve FastScan output equivalence
对于同一个归一化后的 rotated query，fused `quantize + lut_build` 路径 SHALL 生成与参考路径等价的 `fs_width`、`fs_shift` 和 packed LUT bytes。下游 `AccumulateBlock` 与 FastScan distance estimation 的行为 MUST 保持一致。

#### Scenario: Fused LUT bytes match reference layout
- **WHEN** 相同 query 进入 fused 路径与参考两段路径
- **THEN** packed LUT bytes MUST 与当前 `AccumulateBlock` 期望的布局完全一致

#### Scenario: FastScan distances remain equivalent
- **WHEN** fused 路径与参考路径生成的 prepare 结果分别进入 FastScan Stage1 距离估计
- **THEN** 输出距离 MUST 与参考路径等价

### Requirement: LUT build SHALL expose stable internal optimization boundaries
`lut_build` 实现 SHALL 将内部工作划分为稳定的三段边界：group-level 16-entry LUT 生成、lo/hi byte plane 生成、最终 packed lane layout 写出。后续 SIMD 或进一步融合优化 MUST 以这三段为正式落点，而不是继续把所有逻辑混在单一黑盒函数中。

#### Scenario: Internal boundaries remain observable
- **WHEN** 系统在分析或 benchmark 模式下运行优化后的 LUT build
- **THEN** group LUT、byte plane、packed write 三段 MUST 可以被单独定位或观测