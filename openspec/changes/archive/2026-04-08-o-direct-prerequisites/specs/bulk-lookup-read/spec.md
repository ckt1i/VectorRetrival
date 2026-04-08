## ADDED Requirements

### Requirement: Bulk pread for lookup table
ClusterStoreReader::Open() SHALL read the entire lookup table in a single pread call, then parse entries in memory, replacing the current per-field PreadValue pattern.

#### Scenario: Bulk read of lookup table
- **WHEN** ClusterStoreReader::Open() reads the lookup table
- **THEN** it computes total_size = num_clusters × entry_size
- **THEN** it issues a single pread(fd, buf, total_size, lookup_start_offset)
- **THEN** it parses each entry from the in-memory buffer via memcpy

#### Scenario: Bulk read produces identical results
- **WHEN** the same .clu file is read with bulk pread vs the old per-field pread
- **THEN** all lookup table entries (cluster_id, num_records, epsilon, centroid, block_offset, block_size, num_fastscan_blocks, exrabitq_region_offset) are identical
