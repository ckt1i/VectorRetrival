## ADDED Requirements

### Requirement: 4KB-aligned IO buffer allocation
All IO buffers used with io_uring PrepRead SHALL be allocated with 4KB alignment via aligned_alloc(4096, ...).

#### Scenario: Cluster block buffer allocation
- **WHEN** OverlapScheduler allocates a buffer for a cluster block read
- **THEN** the buffer is allocated via aligned_alloc(4096, round_up(size, 4096))
- **THEN** the buffer is freed via free() (not delete[])

#### Scenario: BufferPool allocation
- **WHEN** BufferPool::Acquire allocates a new buffer
- **THEN** the buffer is allocated via aligned_alloc(4096, round_up(capacity, 4096))
- **THEN** BufferPool::Release frees via free() (not delete[])

#### Scenario: aligned_alloc failure
- **WHEN** aligned_alloc returns nullptr
- **THEN** the system aborts with a clear error message (matching existing CheckPrepRead pattern)
