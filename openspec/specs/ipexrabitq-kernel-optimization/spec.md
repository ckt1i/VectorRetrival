# Spec: IPExRaBitQ Kernel Optimization

## Overview

Packed-sign dedicated Stage2 kernel for v10 ExRaBitQ format, with 64-dim block organization and query-centric accumulation.

## Requirements

### Requirement: Packed-sign Stage2 kernel SHALL provide a dedicated fast path
The Stage2 `IPExRaBitQ` implementation SHALL provide a dedicated packed-sign fast path for the formal v10 serving route, instead of requiring the main compute loop to carry the same control flow as legacy byte-sign serving.

#### Scenario: Packed-sign query path selects the dedicated kernel
- **WHEN** Stage2 boosting runs on a candidate whose ExRaBitQ payload uses the v10 packed-sign layout
- **THEN** the system SHALL execute a packed-sign dedicated `IPExRaBitQ` path
- **AND** the query-serving result semantics SHALL remain unchanged

### Requirement: Packed-sign kernel SHALL consume sign bits in 64-dim blocks
The packed-sign dedicated kernel SHALL organize sign consumption around 64-dimension blocks and SHALL derive the sub-block sign masks from each 64-bit sign chunk, rather than rebuilding packed-sign masks independently for every 16-dimension sub-block.

#### Scenario: One 64-dim sign chunk feeds four 16-lane compute blocks
- **WHEN** the packed-sign Stage2 kernel evaluates a 64-dimension block
- **THEN** it SHALL decode one 64-bit sign chunk
- **AND** it SHALL use that chunk to drive the four 16-lane compute sub-blocks inside the same block

### Requirement: Packed-sign kernel SHALL use query-centric accumulation
The packed-sign dedicated kernel SHALL compute Stage2 inner products using a query-centric decomposition that avoids reconstructing a full signed-code float vector.

#### Scenario: Query-centric decomposition preserves numerical result
- **WHEN** the packed-sign kernel computes `Σ q[i] * sgn[i] * (abs[i] + 0.5)`
- **THEN** it SHALL produce a result numerically equivalent to the reference Stage2 formulation
- **AND** it SHALL do so without materializing a full signed float code vector for the candidate

### Requirement: Packed-sign kernel SHALL remain valid across common dimensions
The packed-sign dedicated kernel SHALL support the common serving dimensions used by the project through a shared block structure with correct tail handling, rather than relying on a single fixed-dimension implementation.

#### Scenario: Common dimensions remain supported
- **WHEN** the packed-sign kernel is used on a supported serving dimension such as 96, 128, 256, 384, 512, 768, 1024, or 1536
- **THEN** the kernel SHALL return Stage2 results that preserve the same serving semantics as the reference path

### Requirement: Packed-sign kernel optimization SHALL not require an index rebuild
The first-stage packed-sign kernel optimization SHALL operate on the existing v10 stored ExRaBitQ layout and SHALL NOT require changing the cluster store persistence format.

#### Scenario: Existing v10 index remains usable
- **WHEN** the packed-sign kernel optimization is enabled
- **THEN** an existing v10 packed-sign index SHALL remain queryable without rebuild
