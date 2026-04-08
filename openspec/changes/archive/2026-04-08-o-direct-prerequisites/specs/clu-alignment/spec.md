## ADDED Requirements

### Requirement: .clu v8 file layout with 4KB-aligned cluster blocks
ClusterStoreWriter SHALL produce .clu files with version=8 where every cluster block's offset and the first block's start offset are 4KB-aligned. Padding bytes SHALL be 0x00.

#### Scenario: Writer produces aligned blocks
- **WHEN** ClusterStoreWriter writes a .clu file with N clusters
- **THEN** the lookup table region ends at a 4KB-aligned offset (padded after lookup table)
- **THEN** each cluster block starts at a 4KB-aligned offset (padded after previous block)
- **THEN** the file version field is 8

#### Scenario: Reader opens v8 file
- **WHEN** ClusterStoreReader opens a v8 .clu file
- **THEN** it correctly reads the lookup table and skips padding to locate block data
- **THEN** GetBlockLocation returns 4KB-aligned offsets

#### Scenario: Reader opens v7 file (backward compatibility)
- **WHEN** ClusterStoreReader opens a v7 .clu file
- **THEN** it reads the file correctly with unaligned offsets (existing behavior preserved)

### Requirement: Block read length rounding
When reading a cluster block for O_DIRECT compatibility, the read length SHALL be rounded up to the nearest 4KB multiple.

#### Scenario: SubmitClusterRead with aligned size
- **WHEN** OverlapScheduler reads a cluster block with block_size not a multiple of 4096
- **THEN** the actual read length passed to PrepRead is round_up(block_size, 4096)
- **THEN** ParseClusterBlock uses the original block_size for parsing (not the padded length)
