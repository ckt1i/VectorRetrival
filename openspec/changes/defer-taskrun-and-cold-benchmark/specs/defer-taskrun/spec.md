## ADDED Requirements

### Requirement: io_uring DEFER_TASKRUN initialization
IoUringReader::Init() SHALL attempt to initialize io_uring with IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SINGLE_ISSUER flags in addition to existing IORING_SETUP_CQSIZE flag.

#### Scenario: Kernel supports DEFER_TASKRUN (>= 5.19)
- **WHEN** io_uring_queue_init_params succeeds with DEFER_TASKRUN + SINGLE_ISSUER flags
- **THEN** the ring is initialized with all three flags active and Init() returns Ok

#### Scenario: Kernel does not support DEFER_TASKRUN
- **WHEN** io_uring_queue_init_params fails with -EINVAL due to unsupported flags
- **THEN** Init() retries without DEFER_TASKRUN and SINGLE_ISSUER, using only IORING_SETUP_CQSIZE
- **THEN** Init() returns Ok if the retry succeeds

### Requirement: Poll CQE visibility under DEFER_TASKRUN
When DEFER_TASKRUN is active, Poll() (non-blocking CQE peek) SHALL explicitly trigger kernel task_work processing before peeking CQEs, to ensure completed IOs are visible.

#### Scenario: Poll with DEFER_TASKRUN active
- **WHEN** DEFER_TASKRUN is enabled and IO has completed at hardware level
- **THEN** Poll() calls io_uring_get_events() before io_uring_peek_cqe to flush pending task_work
- **THEN** completed CQEs are visible to the peek loop

#### Scenario: Poll without DEFER_TASKRUN (fallback)
- **WHEN** DEFER_TASKRUN is not enabled (fallback path)
- **THEN** Poll() behavior is unchanged from current implementation (peek only)
