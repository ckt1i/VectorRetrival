## ADDED Requirements

### Requirement: E2E benchmark SHALL expose windowed submit attribution
在线 E2E benchmark MUST 输出足以解释 windowed submit 收益的统计字段。结果中 MUST 至少能够反映 submit 调用次数、submit window 的 flush 次数以及每次 flush 的请求规模，从而区分“submit 次数下降”与“候选量变化”两类影响。

#### Scenario: Windowed submit fields are exported
- **WHEN** benchmark 在启用 windowed submit 的查询路径上运行
- **THEN** 输出中必须包含 submit 次数
- **AND** 必须包含 submit window 的 flush 次数
- **AND** 必须包含用于反映每次 flush 请求规模的统计字段

### Requirement: E2E benchmark SHALL expose batch prepare attribution
在线 E2E benchmark MUST 输出足以解释 batch `PrepRead` 收益的统计字段。结果中 MUST 至少能够区分 `vec_only` 批量 prepare、`all` 批量 prepare、批量 SQE 发射与 `io_uring_submit` 的成本边界。

#### Scenario: Batch prepare breakdown is exported
- **WHEN** benchmark 导出 `probe_submit_ms`
- **THEN** 输出中必须能够区分 `vec_only` 批量 prepare 成本
- **AND** 必须能够区分 `all` 批量 prepare 成本
- **AND** 必须能够区分批量 SQE 发射成本
- **AND** `uring_submit_ms` 仍然必须单独输出
