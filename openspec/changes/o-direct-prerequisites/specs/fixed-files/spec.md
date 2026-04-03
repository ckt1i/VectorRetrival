## ADDED Requirements

### Requirement: io_uring registered file descriptors
IoUringReader SHALL support registering file descriptors via io_uring_register_files, and PrepRead SHALL support using registered fd indices with IOSQE_FIXED_FILE.

#### Scenario: Register files after Init
- **WHEN** RegisterFiles is called with clu_fd and dat_fd after Init
- **THEN** io_uring_register_files registers both fds
- **THEN** subsequent PrepRead calls with fixed_file=true use the registered fd index

#### Scenario: PrepRead with fixed file
- **WHEN** PrepRead is called with fd matching a registered fd and fixed_file=true
- **THEN** the SQE uses the registered fd index instead of the raw fd
- **THEN** sqe->flags includes IOSQE_FIXED_FILE

#### Scenario: PrepRead without fixed file (backward compatibility)
- **WHEN** PrepRead is called with fixed_file=false (default)
- **THEN** behavior is unchanged from current implementation (raw fd, no IOSQE_FIXED_FILE)
