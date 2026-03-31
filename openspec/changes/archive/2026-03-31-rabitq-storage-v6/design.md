# Design: RaBitQ Storage Format v6

## Overview

Transform per-cluster data from flat record-major to dual-region: FastScan blocks + ExRaBitQ entries.

## Binary Layout

### Global Header (unchanged)

```
[magic: u32] [version: u32=6] [num_clusters: u32] [dim: u32]
[rabitq_config: RaBitQConfig]
[data_file_path: length-prefixed string]
```

Version bumped from 5 → 6.

### Lookup Table Entry (extended)

```
[cluster_id: u32]
[num_records: u32]
[epsilon: f32]                  // r_max
[centroid: f32 × dim]
[block_offset: u64]
[block_size: u64]
[num_fastscan_blocks: u32]      // NEW: ceil(num_records / 32)
[exrabitq_region_offset: u32]   // NEW: byte offset within block to Region 2
```

### Per-Cluster Block

```
┌───────────────────────────────────────────────────────────────┐
│ Region 1: FastScan Blocks                                     │
│ (sequential scan, VPSHUFB-friendly)                          │
│                                                               │
│   For b = 0..num_blocks-1:                                    │
│   ┌─────────────────────────────────────────────────────────┐ │
│   │ Block b (32 vectors, or fewer in last block)            │ │
│   │                                                         │ │
│   │ packed_codes: D/4 groups × 32 bytes = D×8 bytes         │ │
│   │   group[m]: 16 bytes low_half + 16 bytes high_half      │ │
│   │   low_half[j]:  nibble = 4 sign bits of vec[j] for      │ │
│   │                 dims [m*4 .. m*4+3]                      │ │
│   │   high_half[j]: same for vec[j+16]                      │ │
│   │                                                         │ │
│   │ factors (32 entries, aligned):                           │ │
│   │   norm_oc[32]: f32 × 32    = 128 bytes                 │ │
│   │                                                         │ │
│   │ Block size: D×8 + 128 bytes                             │ │
│   │ D=96: 768 + 128 = 896 bytes per block                  │ │
│   └─────────────────────────────────────────────────────────┘ │
│                                                               │
│ Total Region 1: ceil(N/32) × fastscan_block_size             │
├───────────────────────────────────────────────────────────────┤
│ Region 2: ExRaBitQ Entries (only when bits > 1)               │
│ (random access per Uncertain vector)                          │
│                                                               │
│   For v = 0..N-1:                                             │
│   ┌─────────────────────────────────────────────────────────┐ │
│   │ ex_code[D]: uint8 × D     = D bytes                    │ │
│   │ ex_sign[D]: uint8 × D     = D bytes                    │ │
│   │ xipnorm:    f32            = 4 bytes                    │ │
│   │                                                         │ │
│   │ Entry size: 2×D + 4 bytes                               │ │
│   │ D=96: 196 bytes per vector                              │ │
│   └─────────────────────────────────────────────────────────┘ │
│                                                               │
│ Total Region 2: N × exrabitq_entry_size (0 if bits=1)        │
├───────────────────────────────────────────────────────────────┤
│ Region 3: Address packed data (unchanged from v5)             │
│ Mini-trailer (unchanged)                                      │
└───────────────────────────────────────────────────────────────┘
```

### Size Comparison (D=96, N=300 vectors per cluster)

```
                    v5 (current)                 v6 (proposed)
────────────────────────────────────────────────────────────────
bits=1:
  Per-cluster:     300 × 24 = 7,200 B          10 blocks × 896 = 8,960 B
  Per-vector avg:  24 B                         ~30 B

bits=4:
  Per-cluster:     300 × 24 = 7,200 B          8,960 + 300×196 = 67,760 B
  (v5 has no S2)   (no ex_code in v5!)
  Per-vector avg:  24 B                         ~226 B

Compared to current exrabitq-alignment (in-memory only):
  bits=4 memory:   300 × (24+196) = 66,000 B   67,760 B (similar)
```

The v6 format for bits=1 is ~25% larger due to block-32 padding + factor storage. For bits>1, it's about the same as current in-memory usage since ex_code is now persisted.

## Data Structure Changes

### ParsedCluster (extended)

```cpp
struct ParsedCluster {
    // --- Region 1: FastScan ---
    const uint8_t* fastscan_blocks;      // pointer to first block
    uint32_t fastscan_block_size;         // bytes per block
    uint32_t num_fastscan_blocks;         // ceil(num_records / 32)

    // --- Region 2: ExRaBitQ ---
    const uint8_t* exrabitq_entries;      // pointer to first entry (nullptr if bits=1)
    uint32_t exrabitq_entry_size;         // 2*D + 4 (0 if bits=1)

    // --- Existing ---
    uint32_t num_records;
    float epsilon;                        // r_max
    std::vector<AddressEntry> decoded_addresses;
    std::unique_ptr<uint8_t[]> block_buf; // owning buffer

    // --- Helpers ---
    // Get norm_oc for vector j in block b
    float norm_oc(uint32_t block_idx, uint32_t vec_in_block) const;
    // Get ex_code/ex_sign/xipnorm for global vector idx
    const uint8_t* ex_code(uint32_t vec_idx) const;
    const uint8_t* ex_sign(uint32_t vec_idx) const;
    float xipnorm(uint32_t vec_idx) const;
};
```

### RaBitQConfig (extended)

```cpp
struct RaBitQConfig {
    uint8_t bits = 1;
    uint32_t block_size = 0;  // unused
    float c_factor = 0.0f;    // unused
    // NEW:
    uint8_t storage_version = 6;  // to distinguish v5 vs v6 on disk
};
```

## Writer Flow

```
ClusterStoreWriter::WriteVectors(codes):

  1. Group codes into blocks of 32

  2. Write Region 1 (FastScan):
     for each block of 32:
       a. Extract sign bits from code[].code (plane 0 / MSB)
       b. Pack into VPSHUFB nibble-interleaved format (pack_codes)
       c. Write packed_codes (D/4 × 32 bytes)
       d. Write factors: norm_oc[32] from code[].norm

  3. Write Region 2 (ExRaBitQ, if bits > 1):
     for each code:
       Write code.ex_code (D bytes)
       Write code.ex_sign (D bytes)
       Write code.xipnorm (4 bytes)
```

### pack_codes detail

For each dim-group of 4 (m = 0..D/4-1):
```
For j = 0..15:
  low_nibble  = sign[perm[j]][m*4+3] | sign[perm[j]][m*4+2]<<1
              | sign[perm[j]][m*4+1]<<2 | sign[perm[j]][m*4+0]<<3
  high_nibble = sign[perm[j]+16][...same...]
  packed[m*32 + j] = low_nibble | (high_nibble << 4)
  packed[m*32 + j+16] = ... (second sub-group)
```

The permutation `perm` matches VPSHUFB lane structure (same as original author's `pack_codes.hpp`).

## Reader Flow

```
ClusterStoreReader::ParseClusterBlock(block_buf):

  1. Read num_fastscan_blocks from lookup entry
  2. fastscan_blocks = block_buf
  3. fastscan_block_size = D/4 * 32 + 32 * sizeof(float)
  4. exrabitq_offset = num_fastscan_blocks * fastscan_block_size
  5. exrabitq_entries = block_buf + exrabitq_offset (if bits > 1)
  6. exrabitq_entry_size = 2*D + 4 (if bits > 1, else 0)
  7. address_data starts after Region 2
```

## Query Path (overlap_scheduler)

```
Current (v5):
  for i in 0..N:
    entry = codes_start + i * code_entry_size
    words = entry → uint64_t*
    norm  = entry + norm_offset
    dist = EstimateDistanceRaw(pq, words, num_words, norm)
    classify(dist)

New (v6):
  for b in 0..num_blocks:
    block = fastscan_blocks + b * block_size

    // Option A: FastScan batch (when fastscan-avx512 is implemented)
    result[32] = AccumulateBlock(block.packed_codes, LUT, D)
    for j in 0..31:
      dist = de_quantize(result[j], block.norm_oc[j], pq)
      classify → if Uncertain, read ex_code from Region 2

    // Option B: Fallback per-vector (until FastScan is ready)
    // Extract sign bits from packed_codes for each vector
    // Use PopcountXor as before
```

This means the storage change is **independent** of FastScan SIMD implementation — the new format can be read both ways.

## Migration

v5 → v6 is a breaking change. Requires:
1. Rebuild index (`ivf_builder` produces v6)
2. Version check on read: reject v5 files with clear error
3. No in-place migration (not needed for benchmark-first approach)

## Files Modified

```
include/vdb/storage/cluster_store.h    — new v6 constants, ParsedCluster
include/vdb/query/parsed_cluster.h     — new fields
src/storage/cluster_store.cpp          — Writer + Reader rewrite
src/query/overlap_scheduler.cpp        — block iteration
src/index/ivf_builder.cpp              — pack codes in v6 format
src/index/crc_calibrator.cpp           — adapt code access
include/vdb/common/types.h            — RaBitQConfig version field
```
