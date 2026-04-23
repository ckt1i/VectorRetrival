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

## ADDED Requirements (from fastscan-prepare-hotpath-optimization)

### Requirement: Fused FastScan prepare SHALL minimize quantize-to-LUT intermediate materialization
在 resident 主工作点的 fused FastScan prepare 路径中，系统 SHALL 尽量缩短 `quantize` 到 `lut_build` 之间的中间态生命周期。实现 MUST 允许量化结果以寄存器或局部 group scratch 形式直接进入 LUT 生成与 packed write，而不要求先将整块 chunk 完整 materialize 为通用中间缓冲再回读。

#### Scenario: Fused prepare avoids full chunk round-trip
- **WHEN** 查询路径执行 fused `quantize + lut_build`
- **THEN** 实现 MUST 允许量化结果直接进入 group LUT 生成
- **AND** 它 MUST 不依赖"完整写出 chunk 中间缓冲后再完整读回"的主路径

### Requirement: Fused FastScan prepare SHALL preserve dimension-agnostic correctness and optimization boundaries
fused FastScan prepare 的主优化路径 MUST 对不同维度工作点保持正确且可用，包括非 `2^n` 维度。实现 MAY 在后续引入受控 specialization，但主路径 MUST 不依赖固定维度快路径才能满足正确性和主要优化边界。

#### Scenario: Generic fused prepare remains valid across dimensions
- **WHEN** 系统在不同维度工作点上执行 fused `quantize + lut_build`
- **THEN** 通用 fused 路径 MUST 生成与参考路径等价的 `fs_width`、`fs_shift` 和 packed LUT bytes
- **AND** 它 MUST 不要求依赖固定维度 specialization 才能成立

### Requirement: Fused FastScan prepare MAY add controlled specialization only as an optional layer
若系统后续引入更宽 chunk、固定维度快路径或其他 specialization，该 specialization SHALL 仅作为通用 fused 路径之上的可选增强层。它 MUST 保持与参考路径等价，并在条件不满足时回退到通用路径。

#### Scenario: Specialization remains optional and reversible
- **WHEN** 系统在某个受控工作点上启用 specialized fused prepare
- **THEN** 生成的 `fs_width`、`fs_shift` 和 packed LUT bytes MUST 与参考路径等价
- **AND** 在不满足 specialization 条件时系统 MUST 仍可使用通用 fused 路径