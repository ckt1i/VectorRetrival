## ADDED Requirements

### Requirement: IOPOLL flag support for io_uring
IoUringReader::Init() SHALL accept a `use_iopoll` parameter. When true, the ring is initialized with IORING_SETUP_IOPOLL.

#### Scenario: Init with IOPOLL enabled
- **WHEN** IoUringReader::Init is called with use_iopoll=true
- **THEN** params.flags includes IORING_SETUP_IOPOLL
- **THEN** WaitAndPoll uses busy-wait polling for CQE completion

#### Scenario: Init with IOPOLL disabled (default)
- **WHEN** IoUringReader::Init is called with use_iopoll=false (default)
- **THEN** behavior is unchanged (interrupt-driven CQE completion)

#### Scenario: IOPOLL fallback
- **WHEN** IOPOLL initialization fails (kernel/hardware unsupported)
- **THEN** Init retries without IOPOLL and succeeds
