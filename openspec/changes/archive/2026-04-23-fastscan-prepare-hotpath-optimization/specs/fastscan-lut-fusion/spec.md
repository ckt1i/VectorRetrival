## ADDED Requirements

### Requirement: Fused FastScan prepare SHALL minimize quantize-to-LUT intermediate materialization
在 resident 主工作点的 fused FastScan prepare 路径中，系统 SHALL 尽量缩短 `quantize` 到 `lut_build` 之间的中间态生命周期。实现 MUST 允许量化结果以寄存器或局部 group scratch 形式直接进入 LUT 生成与 packed write，而不要求先将整块 chunk 完整 materialize 为通用中间缓冲再回读。

#### Scenario: Fused prepare avoids full chunk round-trip
- **WHEN** 查询路径执行 fused `quantize + lut_build`
- **THEN** 实现 MUST 允许量化结果直接进入 group LUT 生成
- **AND** 它 MUST 不依赖“完整写出 chunk 中间缓冲后再完整读回”的主路径

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
