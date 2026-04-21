## ADDED Requirements

### Requirement: Query pipeline SHALL support cluster-end windowed submit
查询管线 MUST 支持在每个 probed cluster 结束时，对当前累计待发 I/O 请求执行 windowed batch submit。系统 MUST 以待提交请求数作为窗口判断依据；当累计待发请求数不超过阈值时，必须一次性提交全部请求；当累计待发请求数超过阈值时，必须按固定粒度分批提交。首版实现 MUST 支持 `8` 或 `16` 作为固定提交粒度。

#### Scenario: Cluster-end flush submits all requests under threshold
- **WHEN** 一个 cluster probe 完成，且当前累计待提交请求数小于或等于 window threshold
- **THEN** 查询管线必须在该 cluster 结束时一次性提交全部待发请求
- **AND** 不得将这些请求继续延迟到后续 cluster

#### Scenario: Cluster-end flush chunks requests over threshold
- **WHEN** 一个 cluster probe 完成，且当前累计待提交请求数大于 window threshold
- **THEN** 查询管线必须按固定粒度分批提交这些请求
- **AND** 每个提交批次的请求数必须不超过当前 window threshold

### Requirement: Query pipeline SHALL preserve CRC and classification semantics under windowed submit
引入 windowed submit 后，查询管线 MUST 保持现有 `SafeIn` / `Uncertain` 分类语义、`VEC_ONLY` / `ALL` 选择语义、CRC cluster-local merge 语义和 early-stop cluster 边界不变。

#### Scenario: Submit timing changes without changing candidate semantics
- **WHEN** 查询路径启用 cluster-end windowed submit
- **THEN** `SafeIn` 候选仍然必须走 `ALL` 路径
- **AND** `Uncertain` 候选仍然必须走 `VEC_ONLY` 路径
- **AND** CRC estimate merge 与 early-stop 判定仍然必须仅在 cluster 边界更新
