## 1. Builder interface and metadata

- [x] 1.1 Add an explicit assignment-mode enum/config to `IvfBuilderConfig` for `single`, `redundant_top2_naive`, and `redundant_top2_rair`
- [x] 1.2 Add RAIR-specific builder parameters, including `lambda` and strict-second-choice behavior
- [x] 1.3 Persist the new assignment mode and RAIR parameters in index metadata
- [x] 1.4 Keep existing `single` and naive redundant build behavior as rollback-compatible modes

## 2. RAIR secondary-assignment implementation

- [x] 2.1 Refactor `DeriveSecondaryAssignments()` so mode selection is explicit instead of inferred only from `assignment_factor`
- [x] 2.2 Implement the naive redundant path as a named strategy for direct comparison
- [x] 2.3 Implement the RAIR secondary-cluster rule using AIR-style residual loss
- [x] 2.4 Ensure RAIR only changes secondary assignment and preserves nearest-centroid primary assignment
- [x] 2.5 Keep the existing duplicated-posting write path unchanged for RAIR-built indexes

## 3. Benchmark and CLI integration

- [x] 3.1 Add benchmark CLI flags for assignment mode and RAIR parameters
- [x] 3.2 Export assignment mode and RAIR parameters in benchmark config and aggregate results
- [x] 3.3 Ensure benchmark output can distinguish `single`, `redundant_top2_naive`, and `redundant_top2_rair`
- [x] 3.4 Export duplication, deduplication, build-time, and preload-cost fields for RAIR runs

## 4. Unit and integration validation

- [x] 4.1 Add builder tests that verify `single`, naive top-2, and RAIR top-2 produce distinct and expected assignment behavior
- [x] 4.2 Add tests that verify RAIR metadata is persisted and reloadable
- [x] 4.3 Add serving-path regression tests that confirm RAIR-built indexes still return unique original vector identities
- [x] 4.4 Add a sanity benchmark that confirms RAIR-built indexes execute successfully on the current redundant serving path

## 5. Experimental evaluation

- [x] 5.1 Run `2048 single` under the current warm-serving protocol as the anchor baseline
- [x] 5.2 Run `2048 redundant_top2_naive` under the same protocol
- [x] 5.3 Run `2048 redundant_top2_rair` under the same protocol
- [x] 5.4 Run a high-recall probing comparison to determine whether RAIR lowers the probing demand needed to reach the target recall region
- [ ] 5.5 If `2048` shows meaningful RAIR gain, extend the same comparison to `1024`

## 6. Reporting and decision

- [x] 6.1 Write the three-way `single / naive / RAIR` comparison into the benchmark result tables
- [x] 6.2 Update analysis notes with the measured effect of RAIR on recall-latency and probing demand
- [x] 6.3 Record build/preload/duplicate overhead alongside the recall gains
- [x] 6.4 Make a keep/stop decision on RAIR based on whether it improves the high-recall trade-off enough to justify the additional complexity
