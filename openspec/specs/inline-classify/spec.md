## ADDED Requirements

### Requirement: ClassifyAdaptive force-inlined
The `ConANN::ClassifyAdaptive` method SHALL be marked `VDB_FORCE_INLINE` and its implementation SHALL be moved from `src/index/conann.cpp` to `include/vdb/index/conann.h`. The function body SHALL remain identical.

#### Scenario: Functional equivalence after inlining
- **WHEN** the inlined ClassifyAdaptive classifies a vector
- **THEN** the result (SafeIn/SafeOut/Uncertain) is identical to the non-inlined version for all inputs

#### Scenario: Existing tests pass
- **WHEN** the ConANN test suite runs
- **THEN** all tests pass with the inlined implementation
