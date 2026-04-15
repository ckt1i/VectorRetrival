# 实验追踪表

**日期**：2026-04-15
**系统**：BoundFetch
**协议**：仅暖稳态

## 基线同步后决策

- 调优后的 `DiskANN+FlatStor` 现已成为 `coco_100k` 上观察到的最强暖服务端前沿。
- 当前 `BoundFetch` 仍明显优于 `FAISS-IVFPQ+FlatStor` 系列，但在与 DiskANN 对比时尚未成为服务端赢家。
- BoundFetch 的下一步**不是**又一轮通用提交路径调优。
- BoundFetch 之前的下一步**是** `.clu` 量化向量和原始地址元数据的暖驻预加载。
- `DiskANN` 保留在基线集中。
- 未来基线计划是分层的；默认情况下不会运行完整的 `search x storage` 笛卡尔矩阵。

## Preload 后方向决策

- `.clu` `full_preload` 已经验证为真实有效的暖稳态优化。
- 当前与 `DiskANN+FlatStor` 的剩余差距，已经不能再主要归因于 cluster 侧的 submit/parse 开销。
- 对于 `recall@10 >= 0.95` 区间，BoundFetch 的下一步优化目标应切换到 probe / verification 的 CPU 路径。
- 主论文对比应开始转向“同步调参后的 IVF 家族基线”，同时保留 DiskANN 作为强图算法上界参考，而不是唯一主目标。
- 更大数据集的扩展是需要的，但应该放在 COCO 100K 主故事稳定之后。

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
| R042 | M5 | 预加载消融 | BoundFetch `preload=off` 当前最优点 | coco_100k | recall@10, e2e_ms, p99_ms, RSS | 必须 | 已完成 | `window` 模式在 `nlist=2048, nprobe=200, alpha=0.05` 下得到 `0.9346 / 2.1224ms / 2.4851ms` |
| R043 | M5 | 预加载消融 | BoundFetch `preload_codes` | coco_100k | recall@10, e2e_ms, p99_ms, RSS, preload_time | 必须 | 已完成 | 当前实现为整份 `.clu` 预加载；测得 preload-on 点为 `0.9346 / 1.6860ms / 2.4900ms`，预加载 `78.352ms`，常驻 `.clu` `120.5MB` |
| R044 | M5 | 预加载消融 | BoundFetch `preload_codes_and_addr` | coco_100k | recall@10, e2e_ms, p99_ms, RSS, preload_time | 必须 | 已完成 | 在本次修订里以 resident 全量 `.clu` 预加载实现，因此量化向量与地址元数据一并加载 |
| R045 | M5 | 分解验证 | BoundFetch 最佳预加载设置 | coco_100k | uring_submit_ms, probe_ms, cluster_parse_ms, RSS | 必须 | 已完成 | `uring_submit_ms: 0.8088 -> 0.1290`，`cluster_parse_ms: 0.0616 -> 0.0000`；收益主要来自移除查询期 cluster submit/parse 开销 |

## M6: 刷新的BoundFetch帕累托

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R046 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=200, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | `0.9640 / 2.9930ms / 4.4790ms` |
| R047 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=200, alpha=0.02` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | `0.9551 / 2.7660ms / 4.2230ms` |
| R048 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=200, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 已测锚点：`0.9346 / 1.6860ms / 2.4900ms` |
| R049 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=200, alpha=0.08` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | `0.9163 / 1.8250ms / 3.5830ms` |
| R050 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=200, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | `0.9021 / 1.7560ms / 3.5680ms` |
| R051 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=500, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | `0.9853 / 4.6650ms / 7.0140ms` |
| R052 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=500, alpha=0.02` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | `0.9843 / 6.1040ms / 8.7120ms` |
| R053 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=500, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | `0.9346 / 1.6950ms / 2.5640ms` |
| R054 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=500, alpha=0.08` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | `0.9171 / 1.8320ms / 3.7680ms` |
| R055 | M6 | 刷新前沿 | BoundFetch 预加载开启 `nprobe=500, alpha=0.1` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | `0.9025 / 1.7560ms / 3.8500ms` |
| R056 | M6 | 合并刷新前沿 | 合并历史和预加载开启的BoundFetch前沿 | coco_100k | 非支配点 | 必须 | 已完成 | `nprobe={200,500}` 与 `alpha={0.01,0.02,0.05,0.08,0.1}` 的 preload-on 网格已补齐，可进入前沿合并与结论整理 |

## M6A: 低负载 Preload 重测

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R056A | M6A | 重测锚点 | BoundFetch `window`, `nprobe=200, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 已完成 | 低负载重测：`0.9346 / 2.4750ms / 3.2250ms` |
| R056B | M6A | 重测锚点 | BoundFetch `full_preload`, `nprobe=200, alpha=0.05` | coco_100k | recall@10, e2e_ms, p99_ms, preload_time, preload_bytes | 必须 | 已完成 | 低负载重测：`0.9346 / 1.6090ms / 1.9460ms`，preload `68.0ms`，常驻 `.clu` `120.5MB` |
| R056C | M6A | 重测高召回点 | BoundFetch `full_preload`, `nprobe=200, alpha=0.02` | coco_100k | recall@10, e2e_ms, p99_ms, preload_time | 必须 | 已完成 | `0.9551 / 2.3440ms / 2.8340ms` |
| R056D | M6A | 重测高召回点 | BoundFetch `full_preload`, `nprobe=200, alpha=0.01` | coco_100k | recall@10, e2e_ms, p99_ms, preload_time | 必须 | 已完成 | `0.9640 / 2.3660ms / 2.8300ms` |

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

## M9: BoundFetch 的 CPU 侧优化

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R067 | M9 | 热路径重新剖析 | BoundFetch `full_preload`, `alpha=0.02` | coco_100k | probe_ms, rerank_cpu_ms, candidate_count | 必须 | 待运行 | 在改代码前重新锚定 `recall@10 >= 0.95` 区间 |
| R068 | M9 | CPU 优化 pass 1 | BoundFetch + 减少重复 decode / check | coco_100k | recall@10, e2e_ms, p99_ms, probe_ms | 必须 | 待运行 | 保持搜索语义不变 |
| R069 | M9 | CPU 优化 pass 2 | BoundFetch + 更适合 SIMD 的 probe 数据布局 | coco_100k | recall@10, e2e_ms, p99_ms, probe_ms | 必须 | 待运行 | 仅在 R068 方向正确时继续 |
| R070 | M9 | CPU 优化 pass 3 | BoundFetch + 降低 candidate materialization / rerank 成本 | coco_100k | recall@10, e2e_ms, p99_ms, rerank_cpu_ms | 必须 | 待运行 | 若收益微弱则停止 |

## M10: 同步 IVF 家族 Pareto

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R071 | M10 | 基线曲线 | 在最终 warm 协议下重新调 FAISS-IVFPQ | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 使用与 BoundFetch 相同的报告流水线 |
| R072 | M10 | 基线曲线 | 同步 sweep 的 IVF+RQ 或 ConANN | coco_100k | recall@10, e2e_ms, p99_ms | 必须 | 待运行 | 选择工程成本最低但可信的第二个 IVF 家族基线 |
| R073 | M10 | 合并 IVF 家族图 | BoundFetch + FAISS + IVF+RQ/ConANN | coco_100k | 非支配点 | 必须 | 待运行 | 作为主家族对比 Pareto 图 |

## M11: 数据集扩展

| 运行ID | 里程碑 | 目的 | 系统/变体 | 数据集 | 指标 | 优先级 | 状态 | 备注 |
|--------|--------|------|----------|--------|------|--------|------|------|
| R074 | M11 | 规模扩展检查 | `deep1m` 或 `deep8m` 上的 BoundFetch 最佳点 | deep* | recall@10, e2e_ms, build_time | 可选 | 待运行 | 仅在 M9-M10 稳定后运行 |
| R075 | M11 | 真实数据检查 | `MS MARCO Passage` 或 `Amazon Products` 上的 BoundFetch + 最强 IVF 家族基线 | real-data | recall@10, e2e_ms, build_time | 可选 | 待运行 | 检查 payload 异质性与真实 schema |

## 当前解读

- 当前最实用的 BoundFetch preload-on 权衡点：`nprobe=200, alpha=0.05`，`0.9346 / 1.609ms / 1.946ms`
- 当前最实用的 BoundFetch preload-on 高召回点：`nprobe=200, alpha=0.02`，`0.9551 / 2.344ms / 2.834ms`
- 当前更高召回的 preload-on 点：`nprobe=200, alpha=0.01`，`0.9640 / 2.366ms / 2.830ms`
- 当前低延迟区域最强 DiskANN 点：`L_search=5`，`0.993 / 1.159ms / 1.901ms`
- 当前实际结论：
  - BoundFetch 已击败观察到的 IVF 家族基线
  - `full_preload` 值得保留，但它没有消除与 DiskANN 的高召回差距
  - 下一步底层杠杆是 CPU 搜索成本，而不是更多 cluster 侧 I/O 路径工作
  - 同步 IVF 家族调参现在比更大规模的数据集扩展更优先

## 决策约束

- 不要从论文中移除DiskANN，除非确实无法进行可复现的集成且该限制有明确文档说明。
- 在分层计划完成之前，不要运行完整的 `search x storage` 矩阵。
- 除非新的 profiling 证据与 preload 结论冲突，否则不要再启动一轮 submit-path 微优化。
- 如果 `R067-R070` 不能创建有意义的新高召回 BoundFetch 区域，就冻结低级优化并把工作重心转向分层权衡故事。
