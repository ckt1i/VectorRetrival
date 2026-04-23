## ADDED Requirements

### Requirement: Query pipeline SHALL support packed-sign ExRaBitQ Stage2 evaluation
查询路径 MUST 支持在 `bits > 1` 且索引使用 packed-sign ExRaBitQ storage 版本时，直接以 packed sign 作为 Stage2 `IPExRaBitQ` 的输入，而不是继续要求逐维 `sign[dim bytes]` 作为查询期主表示。

#### Scenario: Stage2 consumes packed sign directly
- **WHEN** 查询路径在新格式 `.clu` 上执行 Stage2 ExRaBitQ estimation
- **THEN** `IPExRaBitQ` 必须能够直接消费 packed sign
- **AND** 查询路径不得要求解析阶段先把 packed sign 反解为逐维 byte-sign 才能进入 Stage2

### Requirement: Packed-sign query path SHALL preserve current recall and ranking semantics
引入 packed-sign ExRaBitQ storage 后，query pipeline MUST 保持与旧 byte-sign 格式一致的 recall 口径、结果排序语义和 resident serving 语义，不得因为 sign 存储形式变化而改变 top-k 定义。

#### Scenario: New storage format does not change result semantics
- **WHEN** 使用同一 query 集、相同索引内容和相同搜索参数比较 byte-sign 格式与 packed-sign 格式
- **THEN** 查询输出的 recall 定义必须一致
- **AND** 最终 top-k 排序语义必须一致

### Requirement: Packed-sign query path SHALL remain cross-dimension compatible
查询路径中的 packed-sign Stage2 实现 MUST 适用于不同常见维度，而不能只对单一维度硬编码。查询系统 SHALL 在 resident + IVF 路径下继续支持跨维度 block 化 Stage2 执行。

#### Scenario: Packed-sign Stage2 supports multiple dimensions
- **WHEN** 系统在不同常见 embedding 维度上运行 packed-sign ExRaBitQ Stage2
- **THEN** 查询路径必须继续保持正确性
- **AND** 不得要求只在单一维度上才能进入 packed-sign 主路径
