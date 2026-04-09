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
The `PrepareQueryInto` method in `RaBitQEstimator` SHALL call `SimdSubtractAndNormSq` and `SimdNormalizeSignSum` instead of scalar loops for the residual, norm, normalize, sign_code, and sum_q steps. Rotation, quantization, and LUT construction remain unchanged.

#### Scenario: Functional equivalence
- **WHEN** PrepareQueryInto is called with the same query and centroid
- **THEN** the resulting PreparedQuery produces identical distance estimates as the scalar version
