## 1. Address Format Upgrade

- [x] 1.1 Audit the current V8 `.clu` address encoding path and identify every place that assumes `base_offset + prefix_sum(size)` reconstruction
- [x] 1.2 Introduce `AddressColumn V2` data structures for raw page-granularity address entries
- [x] 1.3 Add V2 encode helpers that convert `AddressEntry` into `(offset_pages, size_pages)` entries and validate 4KB alignment
- [x] 1.4 Add V2 decode or accessor helpers sufficient for query-time `AddressEntry` reconstruction without block decompression
- [x] 1.5 Preserve V1 address-column helpers so existing V8 reader tests remain valid

## 2. `.clu` V9 Writer / Reader

- [x] 2.1 Bump the `.clu` file format version and document the V9 address payload layout in code comments
- [x] 2.2 Update `ClusterStoreWriter` so V9 cluster blocks write a contiguous raw address table instead of packed address blocks
- [x] 2.3 Replace the old mini-trailer fields with V2 trailer metadata for V9 blocks
- [x] 2.4 Update `ClusterStoreReader::Open` to branch cleanly between V8 and V9 address parsing
- [x] 2.5 Update `EnsureClusterLoaded` so V9 loads raw address payloads correctly while V8 keeps using packed-address decode
- [x] 2.6 Update `ParseClusterBlockView` so V9 returns a raw address-table view and no longer performs address-block decode
- [x] 2.7 Keep resident preload compatible with both formats and confirm V9 preload no longer depends on address decompression

## 3. Query Path Migration

- [x] 3.1 Extend `ParsedCluster` with V9 raw-address fields and a unified address accessor
- [x] 3.2 Migrate `OverlapScheduler` and related query code to use the unified accessor instead of directly indexing `decoded_addresses`
- [x] 3.3 Recheck the current dedup path and confirm it still collapses duplicated postings before raw-vector fetch submission under V9
- [x] 3.4 Preserve single-assignment behavior as the rollback path for both V8 and V9 indexes

## 4. Builder And Redundant Assignment Integration

- [x] 4.1 Update the builder so redundant postings can reuse shared `data.dat` addresses without any V1 continuity assumptions
- [x] 4.2 Ensure `code[i]`, `address[i]`, original `vec_id[i]`, and CRC/local ids remain aligned in the written cluster order
- [x] 4.3 Add build-side validation that V9 address payload entry count matches `num_records`
- [ ] 4.4 Expose enough metadata for benchmarks to report the new `.clu` / address format version

## 5. Tests And Sanity Validation

- [x] 5.1 Add address-column tests for V2 raw address entries, including gapped address sequences that V1 cannot represent
- [x] 5.2 Add cluster-store roundtrip tests for V9 single-assignment indexes
- [x] 5.3 Add cluster-store roundtrip tests for V9 redundant indexes with shared addresses and gaps
- [x] 5.4 Keep V8 compatibility tests passing
- [x] 5.5 Add query-path regression tests showing redundant-top2 no longer returns catastrophic recall on a controlled dataset
- [x] 5.6 Build and run the targeted test set for address column, cluster store, builder, and overlap scheduler
- [x] 5.7 Run a small sanity benchmark for `2048 redundant_top2` on V9 and confirm recall no longer collapses

## 6. Warm Benchmark Protocol

- [ ] 6.1 Run the fixed warm protocol for `2048` single-assignment on the new V9 format
- [ ] 6.2 Run the fixed warm protocol for `2048 + top-2 redundant assignment` on V9
- [ ] 6.3 Compare whether `2048 + top-2 redundant assignment` improves the `0.99+ recall` region relative to `2048` single-assignment without unacceptable duplication overhead
- [ ] 6.4 Run the fixed warm protocol for `1024` single-assignment on V9
- [ ] 6.5 Run the fixed warm protocol for `1024 + top-2 redundant assignment` on V9
- [ ] 6.6 Compare whether `1024 + top-2 redundant assignment` improves recall enough to justify its additional probe and dedup cost

## 7. Reporting And Decision

- [ ] 7.1 Append all four V9 comparison lines to the active warm benchmark CSV outputs
- [ ] 7.2 Update the benchmark analysis document with the corrected V9 redundant-assignment results and the keep/stop decision
- [ ] 7.3 Record duplication-related system costs, including total probed growth, duplicate candidates, unique fetch candidates, index bytes, and preload bytes
- [ ] 7.4 Decide whether redundant assignment remains in scope as a serving optimization after the first corrected V9 comparison
