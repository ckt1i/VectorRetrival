# Proposal: RaBitQ Storage Format v6 — Dual-Region Layout

## Summary

Redesign the .clu per-cluster data block from record-major to a dual-region layout: Region 1 stores 1-bit codes in FastScan block-32 packed format (VPSHUFB-friendly), Region 2 stores M-bit ExRaBitQ per-vector codes. This enables batch-32 Stage 1 processing and separates the two stages' I/O patterns.

## Problem

Current v5 format stores codes record-major: `[code_words | norm | sum_x]` per vector, sequentially. This has two issues:

1. **Not FastScan-compatible**: VPSHUFB requires codes grouped by dimension-of-4 across 32 vectors. Record-major layout requires runtime repacking.
2. **Mixed concerns**: 1-bit sign codes (Stage 1, sequential scan) and M-bit ExRaBitQ codes (Stage 2, random access for Uncertain only) are interleaved, wasting bandwidth.

## Current Format (v5)

Per-cluster block:
```
Record 0: [code_w0][code_w1][norm][sum_x]   24 bytes (D=96)
Record 1: [code_w0][code_w1][norm][sum_x]
...
Record N-1: ...
[Address packed data]
[Mini-trailer]
```

`code_entry_size = num_words × 8 + 4 + 4`

## Proposed Format (v6)

Per-cluster block:
```
Region 1: FastScan Blocks (Stage 1)
  Block 0: packed_codes[D/4 × 32] + factors[32]
  Block 1: ...
  Block ceil(N/32)-1: (zero-padded)

Region 2: ExRaBitQ Codes (Stage 2, only when bits > 1)
  Vec 0: ex_code[D] + ex_sign[D] + xipnorm
  Vec 1: ...
  Vec N-1: ...

Region 3: Address packed data (unchanged)
Mini-trailer (unchanged)
```

## Affected Code Paths

All locations that read/write `code_entry_size`:

| Component | File | Impact |
|-----------|------|--------|
| Writer | cluster_store.cpp:183-206 | Complete rewrite: write 2 regions |
| Parser (async) | cluster_store.cpp:760-905 | Parse 2 regions, build block pointers |
| Parser (sync) | cluster_store.cpp:597-745 | Same |
| Query probe | overlap_scheduler.cpp:178-187 | Use FastScan batch instead of per-entry |
| CRC calibration | ivf_builder.cpp:577-615, crc_calibrator.cpp:91-161 | Adapt to new layout |
| LoadCode/GetCodePtr | cluster_store.cpp:935-1019 | Index into correct region |

## Scope

### In scope
- .clu format v6 specification
- ClusterStoreWriter: dual-region serialization
- ClusterStoreReader: dual-region parsing
- ParsedCluster struct: add FastScan block pointers
- overlap_scheduler: batch-32 scan interface
- ivf_builder CRC packing: adapt to new layout

### Out of scope
- FastScan VPSHUFB implementation (separate change: fastscan-avx512)
- Backward compatibility with v5 files (breaking change, requires rebuild)
- .dat file format (unchanged)

## Success Criteria

- All unit tests pass
- .clu files written in v6 format can be read back correctly
- overlap_scheduler can iterate blocks of 32 with correct code/norm/xipnorm access
- bench_vector_search produces same recall as v5 (regression check)
