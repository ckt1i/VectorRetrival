## ADDED Requirements

### Requirement: Resident 查询路径必须前移所有 query 无关步骤
当 `bench_e2e` 在 `clu-read-mode=full_preload` 且 `use-resident-clusters=1` 模式下运行时，系统 MUST 在 prepare/init 阶段完成所有 query 无关的 resident cluster 元信息准备、cluster 解析结果构建和在线访问所需辅助结构初始化。在线 query 路径中 SHALL 只保留 query 相关的准备、阈值判断、probe、候选输出、rerank 和最终结果组装。

#### Scenario: Resident 模式进入专用轻路径
- **WHEN** benchmark 以 `--clu-read-mode full_preload --use-resident-clusters 1` 运行
- **THEN** 系统必须进入 resident 专用查询路径，而不是继续走依赖 cluster 在线加载或通用等待/填充逻辑的厚路径

#### Scenario: Query 在线阶段不得重复准备 resident 元数据
- **WHEN** 单个 query 开始执行
- **THEN** 系统不得在该 query 路径中重新构建 resident cluster 解析结果、地址映射或其他 query 无关结构

### Requirement: Resident thin path 必须保留结果语义一致性
resident thin path MUST 保持与现有 resident 查询路径相同的 top-k 结果语义、排序规则和 recall 计算口径，不得因路径变薄而改变最终输出定义。

#### Scenario: Thin path 结果保持一致
- **WHEN** 使用同一索引、同一 query 集和同一参数比较 thin path 与旧 resident 路径
- **THEN** 两者的 recall 口径和最终 top-k 结果语义必须一致