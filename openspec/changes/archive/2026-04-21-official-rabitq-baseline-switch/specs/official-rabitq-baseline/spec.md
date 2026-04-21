## ADDED Requirements

### Requirement: canonical IVF-RQ baseline SHALL use the official RabitQ-Library path
formal-study 的 canonical `IVF+RQ+rerank` baseline SHALL 通过 `baselines/formal-study/scripts/run_vector_search/ivf_rabitq_rerank.py` 及其对应的官方 `RaBitQ-Library` C++ core 执行，而不是走 Faiss `faiss_ivfrq.py` 路径。

#### Scenario: 主 baseline 执行选择官方 RabitQ 路径
- **WHEN** formal-study 运行请求 canonical 的 `IVF-RQ+rerank` baseline
- **THEN** 系统 SHALL 调用 `ivf_rabitq_rerank.py` 执行路径
- **AND** compressed-search kernel SHALL 是官方 `RaBitQ-Library` IVF index

#### Scenario: Faiss RQ 仍然是非 canonical
- **WHEN** formal-study 运行调用 `faiss_ivfrq.py`
- **THEN** 该运行 SHALL 被视为 Faiss reference path
- **AND** 它 SHALL NOT 被视为 canonical 的 `IVF-RQ+rerank` 主比较结果

### Requirement: quantized search SHALL be memory-resident while exact rerank remains disk-backed
官方 RabitQ baseline SHALL 在查询执行时把 quantized / compressed search index 保持在内存中，但 SHALL 在 exact rerank 时从磁盘读取原始向量，而不是依赖预加载的 raw-vector 内存。

#### Scenario: compressed search 使用常驻的官方 index
- **WHEN** 官方 baseline 开始查询
- **THEN** 它 SHALL 将官方 `RaBitQ-Library` IVF index 加载或打开到内存中用于 compressed candidate search

#### Scenario: exact rerank 从磁盘读取原始向量
- **WHEN** 官方 baseline 对一组候选 ID 进行 rerank
- **THEN** 它 SHALL 从磁盘上的 raw-vector 文件中获取对应原始向量
- **AND** rerank 路径 SHALL NOT 要求在查询开始前把所有原始向量预解码进 RAM

### Requirement: canonical baseline timing SHALL include payload fetch
官方 RabitQ baseline SHALL 报告与 payload 对齐的 query latency，其中包括 compressed candidate search、exact raw-vector rerank，以及最终 raw/original payload data 的 fetch。

#### Scenario: 导出包含 payload 的计时
- **WHEN** canonical 的官方 baseline 运行完成
- **THEN** 导出的指标 SHALL 包含一个 query-latency 字段，其计时范围覆盖 compressed search、磁盘 rerank 和 payload/original-data fetch

#### Scenario: metadata 中显式标明计时范围
- **WHEN** canonical baseline 写出结构化结果 metadata
- **THEN** metadata SHALL 显式说明计时范围包含 payload fetch

### Requirement: COCO 100K operating-point tuning SHALL target recall near 0.9 at nprobe 64
对于 COCO 100K 的 canonical 比较设置，在 `nlist=2048` 且 `nprobe=64` 时，官方 RabitQ baseline SHALL 暴露一个调优后的 operating-point 流程，其目标是在 overlap recall 语义下让 `recall@10` 接近 `0.9`。

#### Scenario: 调优流程记录目标与结果
- **WHEN** 官方 RabitQ baseline 在 COCO 100K 上以 `nprobe=64` 进行调优
- **THEN** 实验输出 SHALL 记录目标 recall 区间和实际达到的 `recall@10`
- **AND** 结果 SHALL 保持严格的 overlap recall 语义

#### Scenario: 未达到目标时显式报告
- **WHEN** 在 `nprobe=64` 下调优未能达到目标 recall 区间
- **THEN** 结果摘要 SHALL 显式报告该目标未达成
- **AND** 它 SHALL NOT 静默地把一个更低 recall 的运行呈现为最终 canonical 工作点
