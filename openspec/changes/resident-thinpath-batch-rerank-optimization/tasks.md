## 1. Benchmark 口径修正

- [x] 1.1 梳理 `bench_e2e` 中 CRC calibration 与在线 query round 的现有边界，确认需要保留和需要剥离的运行阶段
- [x] 1.2 修改 `bench_e2e`，使在线 benchmark 仅加载预计算 CRC 参数/工件，不在同一次在线运行中执行 calibration 回退
- [x] 1.3 为 CRC 工件缺失场景补充明确报错或明确的非在线口径处理，并验证 benchmark 输出口径清晰可解释

## 2. Resident Thin Path 实现

- [x] 2.1 识别 resident full-preload 模式下所有可前移到 prepare/init 的 query 无关步骤和数据结构
- [x] 2.2 为 `full_preload + use_resident_clusters=1` 实现 resident 专用 thin path，移除在线路径中不再必要的通用调度/等待/重复解析步骤
- [ ] 2.3 验证 resident thin path 与现有路径在 recall 语义、结果排序和输出定义上保持一致

## 3. 第二阶段测速与 Perf 验证

- [ ] 3.1 使用固定参数运行第二阶段 benchmark：`/home/zcq/VDB/VectorRetrival/build-local/benchmarks/bench_e2e --dataset /home/zcq/VDB/data/coco_100k --output /home/zcq/VDB/test --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.99 --nlist 2048 --nprobe 64 --topk 10 --queries 2000 --bits 4 --clu-read-mode full_preload --use-resident-clusters 1 --early-stop 0`
- [ ] 3.2 对第二阶段实现使用同参数执行 perf 采样并整理 CPU 热点结果，形成阶段二基线

## 4. Batched Rerank 实现

- [x] 4.1 设计并实现候选先收集、后批量 rerank 的执行结构，替代当前候选一到即单条 `L2Sqr` 的模式
- [x] 4.2 调整 raw-vector fetch、top-k 合并与 payload 获取时序，使批量 rerank 不改变 recall 语义和最终结果定义
- [ ] 4.3 为批量 rerank 预留 SIMD 并行化和后续进一步压缩 rerank 常数项的执行基础

## 5. 第三阶段测速与 Perf 验证

- [x] 5.1 使用与第二阶段完全一致的参数运行第三阶段 benchmark，记录速度结果并与阶段二对比
- [x] 5.2 对第三阶段实现执行同口径 perf 采样并整理 CPU 热点结果，形成下一轮优化参考

## 6. 收尾与验收

- [ ] 6.1 汇总 benchmark 口径变更、第二阶段结果和第三阶段结果，确保文档与实现一致
- [ ] 6.2 检查新旧路径切换条件、错误处理和结果统计输出，确保实现具备可持续迭代的分析基础

## 7. 路径保持与统计补充

- [x] 7.1 梳理并保留现有 SafeIn / Uncertain 各自的提交与解码路径，确保实现 batch rerank 时不改变现有预取提交流程
- [x] 7.2 实现全 probe 完成后的统一候选整理、内存池原始向量读取、batch rerank 与 top-k 合并
- [x] 7.3 区分 SafeIn payload 预取与 remaining payload fetch，补充相应时间字段和计数字段
- [x] 7.4 验证新增统计字段输出完整，包括 `prefetch_submit_ms`、`prefetch_wait_ms`、`safein_payload_prefetch_ms`、`candidate_collect_ms`、`pool_vector_read_ms`、`rerank_compute_ms`、`remaining_payload_fetch_ms` 及相关计数字段
- [x] 7.5 明确并验证两类原始向量读取阶段的边界：probe 阶段按 cluster 从磁盘预读到内存池，以及 all-probe-done 后从内存池批量读取再做 rerank
