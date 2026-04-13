# 实验追踪表

**日期**：2026-04-13  
**系统**：BoundFetch  
**协议**：仅暖稳态

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R001 | M0 | 冻结评估范围 | 跨文档更新仅暖协议 | coco_100k | 协议、报告字段 | 必须 | 待运行 | 从活跃计划中移除冷启动相关语言 |
| R002 | M1 | 主帕累托锚点 | BoundFetch `nprobe=50` | coco_100k | recall@10、e2e_ms、p99_ms | 必须 | 已完成 | 现有结果：0.8018 / 0.6391ms |
| R003 | M1 | 主帕累托锚点 | BoundFetch `nprobe=100` | coco_100k | recall@10、e2e_ms、p99_ms | 必须 | 已完成 | 现有结果：0.8748 / 0.9323ms |
| R004 | M1 | 主帕累托锚点 | BoundFetch `nprobe=200` | coco_100k | recall@10、e2e_ms、p99_ms | 必须 | 已完成 | 现有结果：0.8984 / 1.1414ms |
| R005 | M1 | 主帕累托锚点 | BoundFetch `nprobe=300` | coco_100k | recall@10、e2e_ms、p99_ms | 必须 | 已完成 | 现有结果：0.8988 / 1.1530ms |
| R006 | M1 | 主帕累托锚点 | BoundFetch `nprobe=500` | coco_100k | recall@10、e2e_ms、p99_ms | 必须 | 已完成 | 现有结果：0.8988 / 1.1595ms |
| R007 | M1 | 增加帕累托密度 | BoundFetch 中间点（如 `nprobe=150/250`，如可用） | coco_100k | recall@10、e2e_ms、p99_ms | 必须 | 待运行 | 仅在需要明确曲线走势时添加 |
| R008 | M1 | 基线锚点 | DiskANN+FlatStor 最强暖态点 | coco_100k | recall@10、e2e_ms | 必须 | 已完成 | 现有结果：1.000 / 3.7899ms |
| R009 | M1 | 基线锚点 | FAISS-IVFPQ-disk+FlatStor 最强暖态点 | coco_100k | recall@10、e2e_ms | 必须 | 已完成 | 现有结果：0.747 / 1.3124ms |
| R010 | M2 | 机制归因 | BoundFetch 最优点分解 | coco_100k | uring_submit_ms、probe_ms、io_wait_ms、submit_calls | 必须 | 已完成 | nprobe=200 的现有结果已可用 |
| R011 | M2 | 稳定性检验 | BoundFetch 相邻点分解 | coco_100k | 同 R010 | 必须 | 待运行 | 使用 `nprobe=100` 或 `300` 作对比 |
| R012 | M3 | 召回率改进 | 调低 `CRC_alpha` | coco_100k | recall@10、e2e_ms、submit_calls | 必须 | 待运行 | 保持 `io_qd=64`、`mode=shared` 固定 |
| R013 | M3 | 召回率改进 | 调高 `CRC_alpha` | coco_100k | recall@10、e2e_ms、submit_calls | 必须 | 待运行 | 仅改变单一因素 |
| R014 | M3 | 召回率改进 | 放宽裁剪/验证阈值 | coco_100k | recall@10、e2e_ms、probe_ms | 必须 | 待运行 | 目的：测试召回率能否在不产生大延迟损失的情况下提升 |
| R015 | M3 | 简化检验 | 当前最优配置 vs 简化裁剪策略 | coco_100k | recall@10、e2e_ms | 必须 | 待运行 | 决定某些机制是否可以删除 |
| R016 | M4 | 通用性支撑 | BoundFetch 代表点（Deep1M） | deep1m | recall、延迟、分解 | 可选 | 待运行 | 仅在 M1-M3 成功后运行 |
| R017 | M4 | 通用性支撑 | BoundFetch 代表点（Deep8M） | deep8m | recall、延迟、分解 | 可选 | 待运行 | 如果延误写作则跳过 |
| R018 | M4 | 冻结附录检验 | qd sweep 汇总 | coco_100k | e2e_ms | 可选 | 已完成 | 已有 qd=64/128/256/512 结果 |
| R019 | M4 | 冻结附录检验 | shared vs isolated 汇总 | coco_100k | e2e_ms、submit_calls | 可选 | 已完成 | 现有结果可用 |
| R020 | M4 | 冻结附录检验 | SQPOLL fallback 汇总 | coco_100k | e2e_ms、有效标志 | 可选 | 已完成 | 现有 `effective=0` 结果可用 |
