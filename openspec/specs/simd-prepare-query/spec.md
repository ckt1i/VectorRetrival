## ADDED Requirements

### Requirement: SIMD-accelerated residual subtraction and norm computation
The system SHALL provide an AVX-512 function `SimdSubtractAndNormSq` in `include/vdb/simd/prepare_query.h` that computes `residual[i] = a[i] - b[i]` and `norm_sq = Σ residual[i]²` in a single pass using AVX-512 intrinsics. The function SHALL fall back to scalar for non-AVX-512 builds.

#### Scenario: Correct residual and norm for dim=512
- **WHEN** two 512-dim float vectors are subtracted and norm-squared computed
- **THEN** results match scalar implementation within 1e-5 absolute tolerance

#### Scenario: Non-power-of-2 dimensions
- **WHEN** dim is not a multiple of 16
- **THEN** scalar tail loop handles remaining elements correctly

### Requirement: SIMD-accelerated normalize, sign-code, and sum computation
The system SHALL provide an AVX-512 function `SimdNormalizeSignSum` in `include/vdb/simd/prepare_query.h` that, given a vector and its inverse norm, simultaneously normalizes the vector, packs sign bits into a uint64_t array, and computes the element sum. The function SHALL use AVX-512 intrinsics and fall back to scalar for non-AVX-512 builds.

#### Scenario: Sign code generation
- **WHEN** a 512-dim vector is processed
- **THEN** bit i of sign_code is 1 iff the normalized vector element i is >= 0.0

#### Scenario: Sum accuracy
- **WHEN** sum_q is computed for a normalized 512-dim vector
- **THEN** result matches scalar sum within 1e-4 relative tolerance

### Requirement: PrepareQueryInto uses SIMD functions
`RaBitQEstimator` 中的 `PrepareQueryInto` 与 `PrepareQueryRotatedInto` 方法 SHALL 在 residual、norm、normalize、sign-code、sum-q 和 max-abs 这些步骤中调用 SIMD prepare helper，而不是回退到分散的标量阶段。对于 FastScan preparation，实现 SHALL 支持两种合法路径：

- 参考路径：`QuantizeQuery14BitWithMax` 后接 `BuildFastScanLUT`
- 优化路径：fused `quantize + lut_build`

无论选择哪条路径，它 SHALL 针对同一个归一化后的旋转 query 产生等价的 `fs_width`、`fs_shift` 和 LUT contents。Rotation 语义以及由此产生的 distance-estimation 行为 SHALL 保持不变。

当系统启用优化后的 prepare 路径时，FastScan preparation SHALL 支持对子步骤进行独立观测，至少能够区分 `subtract_norm`、`normalize_sign_sum_maxabs`、`quantize` 与 `lut_build` 四段；在进一步细化时，`lut_build` 内部也 SHALL 允许继续分解为 group LUT、byte plane 和 packed write。

#### Scenario: Functional equivalence
- **WHEN** `PrepareQueryInto` 以相同的 query 和 centroid 被调用
- **THEN** 生成的 prepare 结果与 scalar 或变更前实现产生相同的 distance estimates

#### Scenario: Fused FastScan preparation equivalence
- **WHEN** 系统使用 fused 路径为 FastScan 预处理 query
- **THEN** 生成的 `fs_width`、`fs_shift` 和 packed LUT bytes 与参考的 `QuantizeQuery14Bit` 加 `BuildFastScanLUT` 序列输出一致

#### Scenario: Prepared-query substep observability
- **WHEN** 系统在分析或 benchmark 模式下运行优化后的 FastScan preparation
- **THEN** 实现可以分别导出或记录 `subtract_norm`、`normalize_sign_sum_maxabs`、`quantize` 和 `lut_build` 的耗时

### Requirement: PrepareQueryRotatedInto preserves pre-rotated query semantics
`RaBitQEstimator` 中的 `PrepareQueryRotatedInto` 方法 SHALL 继续将 `rotated_q - rotated_centroid` 视为精确的 pre-rotated residual，SHALL 保留足以支撑 Stage1 与 Stage2 的 normalized rotated residual 语义，并 SHALL 在 query-wrapper 模式下准备 Stage1 FastScan 输入，而无需重新引入长期持有的胖 `PreparedQuery` 生命周期。

如果实现采用 scratch buffer、view 或 fused prepare 作为过渡方案，它们 SHALL 只作为局部或 wrapper 持有状态存在，且不应重新引入 query-path 的长期 `quant_query` 必需语义。

#### Scenario: Equivalent Stage 1 preparation for a pre-rotated query
- **WHEN** `PrepareQueryRotatedInto` 以相同的 `rotated_q` 和 `rotated_centroid` 被调用
- **THEN** 生成的 FastScan distance estimates 与变更前准备路径一致

#### Scenario: Equivalent Stage 2 preparation for a pre-rotated query
- **WHEN** `PrepareQueryRotatedInto` 完成一次 pre-rotated query 的准备
- **THEN** `norm_qc`、`norm_qc_sq` 和用于 ExRaBitQ 的 rotated residual 语义仍然可用于后续 estimation

#### Scenario: Query-path temporary quantization state does not escape
- **WHEN** `PrepareQueryRotatedInto` 完成一次 query preparation
- **THEN** 查询期用于构建 LUT 的量化中间态不会作为长期公共语义泄露给下游 probe 路径
