## 1. Resident Compressed-Plane Preload

- [x] 1.1 审查并收口 `ClusterStoreReader::PreloadAllClusters()` 的 resident 数据结构，确认所有 cluster 在 preload 后都具备 probe 所需的量化向量视图与地址映射
- [x] 1.2 新增或重构全局 resident probe-ready cluster 结构，避免 query 期通过 `ToParsedCluster()` 重新复制 `decoded_addresses`
- [x] 1.3 在 `Segment` 层暴露 resident warm path 所需的只读接口，支持查询层直接获取全局 resident cluster 结构
- [x] 1.4 补充 resident preload 相关状态与指标接口，至少覆盖 `clu_preload_time_ms`、`clu_preload_bytes` 和 `resident_cluster_mem_bytes`

## 2. Query Hot Path Simplification

- [x] 2.1 将 `full_preload` 的触发点从 `OverlapScheduler::Search()` 前移到显式 warm-up / benchmark 启动阶段
- [x] 2.2 修改 `OverlapScheduler`，使 resident warm 模式下不再走 `.clu` cluster-block 读取、`ready_clusters_` 等待与 `DispatchCompletion(CLUSTER_BLOCK)` 热路径
- [x] 2.3 修改 probe 路径，使 resident warm 模式直接消费全局 resident cluster 结构，不再为每个 query 重建 resident-local `ParsedCluster`
- [x] 2.4 验证 resident warm 模式下查询期的量化向量搜索完全在内存中执行，且不改变现有搜索语义与 recall

## 3. Benchmark And Validation

- [x] 3.1 扩展 `bench_e2e` 输出，新增 resident warm 模式显式字段：`clu_preload_time_ms`、`clu_preload_bytes`、`resident_cluster_mem_bytes`、`query_uses_resident_clusters`
- [x] 3.2 在 `bench_e2e` 中增加 before/after 对照协议：同一组参数分别运行现有 `full_preload` 路径和新的 resident warm path
- [ ] 3.3 固定第一轮验证参数为 `coco_100k`、`queries=1000`、`topk=10`、`nlist=2048`、`nprobe=200`、`crc-alpha=0.02`、`epsilon-percentile=0.99`
- [ ] 3.4 运行 before/after 实验，记录 `recall@10`、`e2e_ms`、`p99_ms`、resident 指标和搜索速度提升比
- [ ] 3.5 将结果写回 `baselines/results/e2e_comparison_warm.csv` 与 `baselines/results/analysis.md`，明确 resident warm path 带来的收益与额外内存代价

## 4. Regression And Readiness

- [x] 4.1 增加或更新 resident warm 模式的单元/集成验证，确认 query 期不再发生 `.clu` parse 与地址向量复制
- [x] 4.2 回归验证 `window` 与旧 `full_preload` 路径行为不被破坏，并保留可回退能力
- [ ] 4.3 整理一份小结，说明本次 resident warm path 是否足以支撑后续更公平的 baseline 对比与 Pareto 评测
