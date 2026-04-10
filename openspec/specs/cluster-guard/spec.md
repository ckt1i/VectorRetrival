## ADDED Requirements

### Requirement: Cluster-level fast-skip guard
The benchmark hot loop SHALL, before calling `PrepareQueryInto` for each probed cluster, evaluate a lightweight distance lower-bound check using the precomputed `per_cluster_r_max[cid]`. If the lower bound indicates all vectors in the cluster would be classified SafeOut, the cluster SHALL be skipped entirely (no PrepareQueryInto, no FastScan).

#### Scenario: Skip cluster when all vectors are SafeOut
- **WHEN** `centroid_dist[cid] + 2 * per_cluster_r_max[cid] * current_margin_bound < est_kth_threshold`
- **THEN** the cluster is skipped without PrepareQueryInto or FastScan calls

#### Scenario: Do not skip when vectors may be in top-K
- **WHEN** a cluster contains vectors within the decision boundary
- **THEN** the guard condition evaluates false and normal processing continues

### Requirement: Guard must not reduce recall
The cluster-level guard SHALL use a conservative distance lower bound that never excludes a cluster containing a true top-K neighbor. Recall@10 SHALL NOT decrease by more than 0.001 compared to the baseline without the guard.

#### Scenario: Recall preservation
- **WHEN** the guard is enabled on the benchmark with CRC
- **THEN** recall@10 remains >= baseline recall - 0.001
