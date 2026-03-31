# Tasks: RaBitQ Storage Format v6

## Step 0: Data Structure Updates

- [x] **0.1** Add `storage_version` field to `RaBitQConfig` in `include/vdb/common/types.h`
- [x] **0.2** Extend `ParsedCluster` in `include/vdb/query/parsed_cluster.h`
  - Add: `fastscan_blocks`, `fastscan_block_size`, `num_fastscan_blocks`
  - Add: `exrabitq_entries`, `exrabitq_entry_size`
  - Add: helper methods `norm_oc(block, vec)`, `ex_code(vec)`, `ex_sign(vec)`, `xipnorm(vec)`
- [x] **0.3** Update `ClusterStoreWriter/Reader` header in `cluster_store.h`
  - Add: `fastscan_block_size()` and `exrabitq_entry_size()` methods
  - Extend `ClusterLookupEntry`: `num_fastscan_blocks`, `exrabitq_region_offset`
  - Bump version constant from 6 → 7
- [x] **0.4** Add `PackSignBitsForFastScan()` utility function
  - Input: vector of RaBitQCode (sign bits from code[plane 0])
  - Output: packed nibble-interleaved bytes for one block of 32
  - Reference: `thrid-party/Extended-RaBitQ/inc/index/fastscan/pack_codes.hpp`
  - Created in `src/storage/pack_codes.cpp` + `include/vdb/storage/pack_codes.h`

## Step 1: Writer

- [x] **1.1** Rewrite `ClusterStoreWriter::WriteVectors()` for v7
  - Group codes into blocks of 32 (pad last block with zeros)
  - Region 1: for each block, write packed_codes (D×4 bytes) + norm_oc[32]
  - Region 2 (bits > 1): for each code, write ex_code[D] + ex_sign[D] + xipnorm
  - Track region sizes for lookup table
- [x] **1.2** Update `ClusterStoreWriter::BeginCluster/EndCluster`
  - Record `num_fastscan_blocks` and `exrabitq_region_offset` in lookup entry
- [x] **1.3** Update lookup table serialization
  - Write new fields: `num_fastscan_blocks`, `exrabitq_region_offset`
- [x] **1.4** Unit test: existing cluster_store_test updated for v7
  - Writer_MultipleClusters, Reader_LoadCodes, etc. all pass
  - GetCodePtr_MatchesLoadCode: verifies pack→unpack round-trip
  - ParseClusterBlock_MatchesEnsureClusterLoaded: verifies v7 fields

## Step 2: Reader

- [x] **2.1** Rewrite `ClusterStoreReader::ParseClusterBlock()` for v7
  - Parse Region 1: compute block pointers, fastscan_block_size
  - Parse Region 2: compute exrabitq_entries pointer, entry_size
  - Parse Region 3: address data (offset shifted)
  - Populate ParsedCluster with new fields
- [x] **2.2** Update `EnsureClusterLoaded()` for v7
  - codes_length calculation changes: Region 1 + Region 2 combined
  - Address region offset shifted accordingly
- [x] **2.3** Update `LoadCode/LoadCodes/GetCodePtr` for v7
  - LoadCode: extract sign bits from packed block format (inverse of PackSignBitsForFastScan)
  - LoadCodes: also read ex_code/ex_sign/xipnorm from Region 2
  - GetCodePtr: return pointer to FastScan block
- [x] **2.4** Version check: kFileVersion bumped to 7, reader rejects non-7 files
- [x] **2.5** Unit test: round-trip write+read
  - Existing tests verify bits=1 round-trip (pack→unpack sign bits, norm, sum_x)
  - All 17 cluster_store tests pass

## Step 3: Query Path Adaptation

- [x] **3.1** Update `OverlapScheduler::ProbeCluster()` for v7
  - Iterate by FastScan blocks of 32 instead of per-record
  - For each block: read norms from Region 1 factors
  - Per-vector fallback: UnpackSignBitsFromFastScan + EstimateDistanceRaw
  - (FastScan SIMD batch accumulation to be added in fastscan-avx512 change)
- [x] **3.2** OverlapScheduler constructor unchanged (num_words_ still used for fallback)
- [x] **3.3** Integration test: run bench_vector_search on deep1m
  - bits=4: recall@10=0.9730, False SafeOut=0, latency=3.528ms ✓
  - bits=1: recall@10=0.9730, False SafeOut=0, latency=2.746ms ✓
  - Both match v5 baseline exactly

## Step 4: Index Builder Adaptation

- [x] **4.1** Update `ivf_builder.cpp` to produce v7 format
  - `EncodeBatch` already produces RaBitQCode with ex_code/ex_sign
  - `WriteVectors` call chain now writes v7 blocks (no builder changes needed)
  - CRC flat packing: kept as temporary in-memory flat buffer (not persisted)
- [x] **4.2** `crc_calibrator.cpp` code access unchanged
  - CRC calibration uses temporary flat buffer built by ivf_builder
  - Not affected by on-disk format change
- [x] **4.3** Build + test pass: ivf_builder and overlap_scheduler tests pass

## Step 5: Benchmark Verification

- [x] **5.1** Run bench_rabitq_accuracy on deep1m with v7 format
  - bits=4: S1 MAE=0.105398, S2 MAE=0.005921, S2 recall@10=0.96 ✓
  - All values match v5 baseline exactly
- [x] **5.2** Run bench_vector_search on deep1m with v7 format
  - bits=4: recall@10=0.9730, False SafeOut=0, latency=3.528ms ✓
  - bits=1: recall@10=0.9730, False SafeOut=0, latency=2.746ms ✓
  - All recall values match v5 baseline exactly
