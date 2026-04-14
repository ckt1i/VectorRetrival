## MODIFIED Requirements

### Requirement: Cluster-side query data SHALL support selectable loading modes
The cluster-side query path SHALL support both the existing sliding-window preload mode and a full `.clu` preload mode, with the loading mode selected explicitly by configuration.

#### Scenario: Sliding-window mode remains available
- **WHEN** the cluster loading mode is set to `window`
- **THEN** the query pipeline SHALL continue to use the existing sliding-window cluster-read path
- **AND** it SHALL preserve the current `prefetch_depth` and refill-based behavior

#### Scenario: Full preload mode bypasses cluster-block submissions
- **WHEN** the cluster loading mode is set to `full_preload`
- **THEN** the query pipeline SHALL obtain cluster-side probe data from resident memory
- **AND** it SHALL NOT submit per-cluster `CLUSTER_BLOCK` I/O requests during the measured query phase

#### Scenario: Mode selection is explicit and comparable
- **WHEN** the benchmark or query pipeline is configured for cluster loading
- **THEN** the selected mode SHALL be externally visible in configuration or output metadata
- **AND** the same query parameters SHALL be runnable under both modes for before/after comparison

### Requirement: Query-time cluster probe SHALL remain compatible with the parsed-cluster access pattern
Regardless of loading mode, the cluster probe path SHALL consume a cluster representation that exposes the same probe-relevant fields required by the current parsed-cluster-based logic.

#### Scenario: Probe path uses a compatible resident representation
- **WHEN** full preload mode is enabled
- **THEN** the cluster probe implementation SHALL receive a resident cluster representation compatible with the existing quantized-code and decoded-address access pattern
- **AND** the change SHALL NOT require a different search algorithm or a different top-k interpretation

## REMOVED Requirements

### Requirement: Sliding-window prefetch is the only cluster-side loading strategy
**Reason**: The system now needs a second mode that fully preloads `.clu` data before queries so the project can compare query-time speed against the previous sliding-window strategy.
**Migration**: Existing behavior remains available under `window` mode. New benchmarks and implementations SHALL treat sliding-window loading as one selectable mode rather than the only supported strategy.
