## ADDED Requirements

### Requirement: E2E benchmark SHALL expose ExRaBitQ storage version metadata
当 benchmark 在 `bits > 1` 的 ExRaBitQ 索引上运行时，输出 MUST 显式记录当前索引使用的 ExRaBitQ storage 版本或等价格式标识，以便区分旧的 byte-sign 布局和新的 packed-sign 布局。

#### Scenario: Benchmark output records storage version
- **WHEN** `bench_e2e` 在 ExRaBitQ 索引上完成一次运行并导出结果
- **THEN** 结果中必须包含 ExRaBitQ storage version 或等价格式标识
- **AND** 该字段必须足以区分 byte-sign 与 packed-sign 两种布局

### Requirement: Benchmark SHALL validate packed-sign format under both clean perf and full E2E
针对 ExRaBitQ storage 升级，benchmark MUST 同时提供 query-only clean perf 与 full E2E 两个口径的验收结果，以验证 packed-sign 是否真实降低了 Stage2 成本，并确认结果语义未退化。

#### Scenario: Query-only benchmark can attribute Stage2 benefit
- **WHEN** packed-sign 格式与旧格式进行 query-only perf 对比
- **THEN** 输出必须能够用于判断 Stage2 热点占比是否迁移
- **AND** 不得把该对比建立在被细粒度打点污染的口径上

#### Scenario: Full E2E benchmark validates semantic compatibility
- **WHEN** packed-sign 格式与旧格式进行 full E2E 对比
- **THEN** 输出必须包含 recall 和端到端时延字段
- **AND** 必须能够确认 packed-sign 未改变结果语义

### Requirement: Benchmark SHALL make rebuild / format provenance explicit
当 benchmark 运行在升级后的 ExRaBitQ storage 格式上时，输出 MUST 能区分该结果来自旧格式索引还是新格式重建索引，并为后续 perf / 实验解释提供格式来源信息。

#### Scenario: Output distinguishes rebuilt packed-sign index from legacy index
- **WHEN** benchmark 导出一条 ExRaBitQ 结果记录
- **THEN** 结果中必须能区分该运行使用的是 legacy byte-sign 索引还是 rebuilt packed-sign 索引
- **AND** 该区分必须可用于后续分析和对照
