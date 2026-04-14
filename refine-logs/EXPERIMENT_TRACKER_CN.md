# 实验追踪表

**日期**：2026-04-14
**系统**：BoundFetch
**协议**：仅暖稳态

## 基线同步后决策

- 调优后的 `DiskANN+FlatStor` 现已成为 `coco_100k` 上观察到的最强暖服务端前沿。
- 当前 `BoundFetch` 仍明显优于 `FAISS-IVFPQ+FlatStor` 系列，但在与 DiskANN 对比时尚未成为服务端赢家。
- BoundFetch 的下一步**不是**又一轮通用提交路径调优。
- BoundFetch 的下一步**是** `.clu` 量化向量和原始地址元数据的暖驻预加载。
- `DiskANN` 保留在基线集中。
- 未来基线计划是分层的；默认情况下不会运行完整的 `search x storage` 笛卡尔矩阵。

## 同步规则

- 所有帕累托运行必须在 `coco_100k` 上使用相同的仅暖协议。
- 所有系统必须使用 `queries=1000` 和 `topk=10`。
- 即使 DiskANN 当前主导前沿，它仍是必需的强参考。
- 基线扩展必须遵循分层策略：
  - 主搜索核心对比
  - 存储后端消融
  - 构建/预加载成本表

## 历史主暖帕累托结果

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R002 | M1 | 主帕累托锚点 | BoundFetch `nlist=2048, nprobe=50, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.8068 / 0.743ms / 1.001ms |
| R003 | M1 | 主帕累托锚点 | BoundFetch `nlist=2048, nprobe=100, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.8781 / 1.117ms / 1.513ms |
| R004 | M1 | 主帕累托锚点 | BoundFetch `nlist=2048, nprobe=200, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.9021 / 1.409ms / 2.493ms |
| R005 | M1 | 主帕累托锚点 | BoundFetch `nlist=2048, nprobe=300, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.9025 / 1.393ms / 2.824ms |
| R006 | M1 | 主帕累托锚点 | BoundFetch `nlist=2048, nprobe=500, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.9025 / 1.403ms / 2.773ms |
| R012 | M3 | 召回率改进最优点 | BoundFetch `nlist=2048, nprobe=200, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.9346 / 1.511ms / 1.855ms |
| R021 | M1A | 高召回率点 | BoundFetch `nlist=2048, nprobe=200, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.9640 / 2.251ms / 2.648ms |
| R022 | M1A | 高召回率点 | BoundFetch `nlist=2048, nprobe=500, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.9853 / 3.788ms / 4.313ms |

## 历史基线帕累托结果

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R025 | M1B | 基线曲线 | DiskANN+FlatStor `L_search=5` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.993 / 1.159ms / 1.901ms |
| R026 | M1B | 基线曲线 | DiskANN+FlatStor `L_search=10` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.993 / 1.374ms / 2.187ms |
| R027 | M1B | 基线曲线 | DiskANN+FlatStor `L_search=15` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.995 / 1.395ms / 2.223ms |
| R028 | M1B | 基线曲线 | DiskANN+FlatStor `L_search=20` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.997 / 1.925ms / 2.975ms |
| R033 | M1B | 基线曲线 | FAISS-IVFPQ+FlatStor `nprobe=8` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.715 / 0.333ms / 0.491ms |
| R034 | M1B | 基线曲线 | FAISS-IVFPQ+FlatStor `nprobe=16` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.745 / 0.657ms / 0.935ms |
| R035 | M1B | 基线曲线 | FAISS-IVFPQ+FlatStor `nprobe=32` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 0.747 / 1.317ms / 1.658ms |
| R041 | M1B | 合并帕累托表 | BoundFetch + DiskANN + FAISS | coco_100k | 非支配点 | 必须 | 已完成 | 合并CSV位于 `baselines/results/` |

## 历史机制归因

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R010 | M2 | 机制归因 | BoundFetch `nprobe=200, alpha=0.1` | coco_100k | uring_submit_ms, probe_ms, io_wait_ms, submit_calls | 必须 | 已完成 | `uring_submit_ms` 占主导；`io_wait_ms` 可忽略 |
| R011 | M2 | 稳定性检验 | BoundFetch `nprobe=300, alpha=0.1` | coco_100k | 同R010 | 必须 | 已完成 | 相同结论 |

## M5: 暖驻预加载实现

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R042 | M5 | 预加载消融 | BoundFetch `preload=off` 当前最优点 | coco_100k | recall@10, e2e_ms, p99_ms, RSS | 必须 | 待运行 | 预加载对比基线 |
| R043 | M5 | 预加载消融 | BoundFetch `preload_codes` | coco_100k | recall@10, e2e_ms, p99_ms, RSS, preload_time | 必须 | 待运行 | 仅缓存量化向量 |
| R044 | M5 | 预加载消融 | BoundFetch `preload_codes_and_addr` | coco_100k | recall@10, e2e_ms, p99_ms, RSS, preload_time | 必须 | 待运行 | 缓存量化向量和原始地址元数据 |
| R045 | M5 | 分解验证 | BoundFetch 最佳预加载设置 | coco_100k | uring_submit_ms, probe_ms, cluster_parse_ms, RSS | 必须 | 待运行 | 验证热路径上的变化 |

## M6: 刷新的BoundFetch帕累托

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R046 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=200, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 带预加载的高召回锚点 |
| R047 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=200, alpha=0.02` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 近高召回点 |
| R048 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=200, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 带预加载的当前最佳权衡点 |
| R049 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=200, alpha=0.08` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 前沿密度 |
| R050 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=200, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 与历史默认对比 |
| R051 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=500, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 带预加载的最高召回锚点 |
| R052 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=500, alpha=0.02` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 高召回点 |
| R053 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=500, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 权衡点 |
| R054 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=500, alpha=0.08` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 前沿密度 |
| R055 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=500, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 与历史默认对比 |
| R056 | M6 | 合并刷新前沿 | 合并历史和预加载开启的BoundFetch前沿 | coco_100k | 非支配点 | 必须 | 待运行 | 进一步优化决策门控 |

## M7: 分层基线对比

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R057 | M7 | 主搜索核心表 | 选择主后端策略 | coco_100k | 可比性说明 | 必须 | 待运行 | 如果可行，优先选择共享 `Lance` 后端 |
| R058 | M7 | 主搜索核心表 | 已选策略上BoundFetch最佳预加载点 | coco_100k | recall@10, e2e_ms, p99_ms, build_time | 必须 | 待运行 | 主表点 |
| R059 | M7 | 主搜索核心表 | 已选策略上IVF+PQ或IVF+RQ | coco_100k | recall@10, e2e_ms, p99_ms, build_time | 必须 | 待运行 | 较低经典基线 |
| R060 | M7 | 主搜索核心表 | 已选策略上DiskANN或作为 `DiskANN+FlatStor` 上参考 | coco_100k | recall@10, e2e_ms, p99_ms, build_time | 必须 | 待运行 | 必须保留在范围内 |
| R061 | M7 | 存储消融 | BoundFetch 在 `FlatStor` 和 `Lance` 上对比 | coco_100k | recall@10, e2e_ms, startup_time, file_size | 必须 | 待运行 | 仅从2个后端开始 |
| R062 | M7 | 存储消融 | IVF基线在 `FlatStor` 和 `Lance` 上对比 | coco_100k | recall@10, e2e_ms, startup_time, file_size | 必须 | 待运行 | 将后端效应与搜索核心效应分离 |
| R063 | M7 | 可选存储消融 | 如果集成成本低廉则添加 `Parquet` | coco_100k | recall@10, e2e_ms, startup_time, file_size | 可选 | 待运行 | 不要阻塞主故事 |

## M8: 构建和启动成本表

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R064 | M8 | 构建成本表 | BoundFetch构建和预加载成本 | coco_100k | build_time, peak_RSS, index_bytes, preload_bytes, preload_time | 必须 | 待运行 | 即使DiskANN仍在服务上获胜也需要 |
| R065 | M8 | 构建成本表 | DiskANN构建和启动成本 | coco_100k | build_time, peak_RSS, index_bytes, preload_bytes, preload_time | 必须 | 待运行 | 强对比轴 |
| R066 | M8 | 构建成本表 | IVF家族基线构建和启动成本 | coco_100k | build_time, peak_RSS, index_bytes, preload_bytes, preload_time | 必须 | 待运行 | 完成权衡表 |

## 当前解读

- 当前BoundFetch最佳权衡点：`nprobe=200, alpha=0.05`，`0.9346 / 1.511ms / 1.855ms`
- 当前BoundFetch高召回点：`nprobe=200, alpha=0.01`，`0.9640 / 2.251ms / 2.648ms`
- 当前低延迟区域最强DiskANN点：`L_search=5`，`0.993 / 1.159ms / 1.901ms`
- 当前实际结论：
  - BoundFetch击败IVF家族基线
  - DiskANN仍是暖服务端上参考基线
  - preload是最高价值的下一步测试

## 决策约束

- 不要从论文中移除DiskANN，除非确实无法进行可复现的集成且该限制有明确文档说明。
- 在分层计划完成之前，不要运行完整的 `search x storage` 矩阵。
- 在 `R042-R056` 完成之前，不要开始又一轮提交路径微优化。
- 如果 `R042-R056` 未能创建有用的新BoundFetch前沿区域，则冻结低级优化并将工作重心转向分层权衡故事。
