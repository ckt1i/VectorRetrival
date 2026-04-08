## ADDED Requirements

### Requirement: O_DIRECT flag support for .clu and .dat files
ClusterStoreReader::Open() and DataFileReader::Open() SHALL accept a `use_direct_io` parameter. When true, files are opened with O_RDONLY | O_DIRECT.

#### Scenario: Open with O_DIRECT enabled
- **WHEN** Segment::Open is called with use_direct_io=true on a v8 .clu file
- **THEN** both .clu and .dat file descriptors are opened with O_RDONLY | O_DIRECT
- **THEN** all subsequent pread/io_uring reads go through DMA direct path (no page cache)

#### Scenario: Open with O_DIRECT disabled (default)
- **WHEN** Segment::Open is called with use_direct_io=false (default)
- **THEN** files are opened with O_RDONLY only (existing behavior)

#### Scenario: Header and lookup table reading under O_DIRECT
- **WHEN** ClusterStoreReader::Open reads the header and lookup table with O_DIRECT fd
- **THEN** it uses an aligned buffer for the bulk pread
- **THEN** parsing succeeds identically to non-O_DIRECT path

#### Scenario: bench_e2e --direct-io flag
- **WHEN** bench_e2e is invoked with --direct-io
- **THEN** Segment::Open is called with use_direct_io=true
