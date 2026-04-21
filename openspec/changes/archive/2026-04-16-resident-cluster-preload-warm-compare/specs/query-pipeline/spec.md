## MODIFIED Requirements

### Requirement: OverlapScheduler
The query pipeline SHALL support both sliding-window cluster loading and a true resident warm path. Under resident warm mode, preload SHALL be completed before measured queries begin, and the query hot path SHALL probe clusters directly from globally resident cluster structures without query-time `.clu` I/O, cluster parse, or query-local resident materialization.

#### Scenario: 端到端小规模搜索
- Given 一个已构建的 IVF 索引（N=1000, dim=128, nlist=4）
- And PreadFallbackReader
- When Search(query_vec, top_k=10, nprobe=2)
- Then 返回 10 个 SearchResult
- And 结果按 distance 升序排列
- And stats.total_probed > 0
- And stats.total_io_submitted > 0

#### Scenario: 全部 SafeOut 时不发 I/O
- Given 一个索引，且 ConANN epsilon 极大（所有 candidate 都 SafeOut）
- When Search(query_vec, top_k=10, nprobe=1)
- Then 返回 0 个结果
- And stats.total_io_submitted == 0

#### Scenario: nprobe=1 单 cluster
- Given 一个索引（nlist=4, cluster_0 有 100 条记录）
- When Search(query_vec, top_k=5, nprobe=1)
- Then 返回 min(5, non-SafeOut count) 个结果
- And 所有结果的 vec_id 解码后 cluster_id == 最近 cluster

#### Scenario: Probe + I/O overlap
- Given 一个索引（nlist=8, 每 cluster 200 条）
- And probe_batch_size=64, io_batch_size=32
- When Search(query_vec, top_k=10, nprobe=4)
- Then 完成搜索（不死锁、不遗漏）
- And stats.total_probed == sum of non-SafeOut cluster sizes

#### Scenario: Resident warm mode bypasses cluster read scheduling
- Given 一个已完成 resident warm-up 的索引
- And 查询配置启用了 resident cluster warm mode
- When Search(query_vec, top_k=10, nprobe=4)
- Then 查询热路径 SHALL 直接从 resident cluster 结构获取 probe 输入
- And stats.total_io_submitted SHALL 不包含任何 `.clu` cluster-block 读取
- And 查询过程中 SHALL 不等待 `ready_clusters_` 中的 cluster block 完成

#### Scenario: Resident warm mode does not lazily trigger preload in first measured query
- Given benchmark 在 measured query batch 之前已经显式执行 resident warm-up
- When 第一条 measured query 开始执行
- Then 查询路径 SHALL 直接使用已存在的 resident cluster 状态
- And 这条 measured query SHALL 不承担 `.clu` preload 时间
