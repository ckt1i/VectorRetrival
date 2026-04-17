# Baseline 方法方案

日期：2026-04-16

## 1. baseline 总览

正式 baseline 分成三类：

| 类别 | 方法 | 角色 |
|------|------|------|
| 主 baseline | Faiss IVFPQ + rerank | 最基础的 IVF+rerank 对照 |
| 主 baseline | IVF + RaBitQ + rerank | 与当前方法同属 RabitQ 路线的主对照 |
| 强参考 | DiskANN | 图索引上界参考与 build/startup 对照 |
| 可选增强 | ConANN + Faiss IVF | 验证“自适应 cluster probing”这一类贡献 |

## 2. 必须落地的主 baseline

### 2.1 Faiss IVFPQ + rerank

定义：

- 第一阶段使用 `IndexIVFPQ`
- 第二阶段对候选做 exact rerank

作用：

- 对标你现在的“量化初筛 + rerank”结构
- 避免只拿一个不带 refine 的弱 PQ baseline 来比较

实现要求：

- 候选集大小需要记录
- rerank 阶段的 top-k 和 candidate budget 要固定
- 在 disk 模式下继续使用你现有的 payload 对接方式
- 正式对比口径下，量化 bit 固定为 `4`

### 2.2 IVF + RaBitQ + rerank

定义：

- IVF coarse assignment
- `RaBitQ` 量化向量用于候选评分
- exact rerank 仍从磁盘原始向量读取

作用：

- 对标“同属 RabitQ 量化路线但不包含 BoundFetch 系统协同优化”的基线
- 把“量化器本身收益”和“BoundFetch 的系统协同收益”拆开

实现要求：

- 当前按“自建 baseline”规划，后续单开 propose 实现
- 保持磁盘 rerank 与 payload 协议和其他 baseline 一致
- 正式对比口径下，量化 bit 固定为 `4`

### 2.3 不再纳入正式 baseline 的方法

以下方法不再进入本轮正式 baseline 主套餐：

- `Faiss IVFPQR`
- `Faiss IVFResidualQuantizer`
- `Faiss IVFLocalSearchQuantizer`

原因：

- 当前正式问题已经收敛到 `IVF+PQ` 与 `IVF+RaBitQ` 两条主线
- `ResidualQuantizer` 系方法不再是后续论文主叙事的一部分
- `IVFPQR` 更接近 PQ 家族内部增强项，不再作为独立主 baseline

## 3. 可选增强 baseline

### 3.1 ConANN + Faiss IVF

定位：

- 验证“自适应 probing / conformal control”这类收益是否独立存在

适用场景：

- 当你想把 `cluster probing 控制` 这部分贡献单独拿出来解释时

是否主表必需：

- 否
- 建议作为增强 baseline 或机制表中的方法对照

## 4. DiskANN 的定位

DiskANN 不建议移出论文，但不建议继续当作主 baseline 套餐中的唯一焦点。

建议定位：

- 图索引上界参考
- 高召回 latency 目标
- build/startup 成本对照

不建议定位：

- 只要没打赢 DiskANN，主方法就不成立

## 5. 存储后端方案

存储后端固定为：

- FlatStor
- Lance
- Parquet

建议策略：

- 主表只选 1 个主后端
- storage backend ablation 单独出图

布局口径固定为：

- `Lance`：原始向量与原始数据作为两列存入同一 Lance 数据集
- `FlatStor`：正式口径下只存原始数据，原始向量不并入同一文件
- `Parquet`：正式口径下只存原始数据，原始向量不并入同一文件

补充规则：

- 如果 `Lance` 官方库支持 `IVF + RaBitQ`，则 `Lance` 侧端到端搜索统一直接使用 Lance 原生实现
- 在该条件满足时，不再对 Lance 额外叠加 Faiss 或自建 IVF+RaBitQ 搜索核

推荐优先级：

1. FlatStor
2. Lance
3. Parquet

原因：

- FlatStor 与现有系统最贴近
- Lance 具有更强生产感
- Parquet 更多是通用列存参考

## 6. 每个 baseline 需要统一记录的字段

### 搜索核字段

- `system`
- `dataset`
- `metric`
- `nlist`
- `nprobe`
- `topk`
- `candidate_budget`
- `recall@1`
- `recall@10`
- `recall@20`
- `avg_ms`
- `p50_ms`
- `p95_ms`
- `p99_ms`
- `qps`

### 构建字段

- `build_time_s`
- `peak_rss_mb`
- `index_bytes`
- `train_time_s`

### 启动字段

- `open_time_ms`
- `preload_time_ms`
- `preload_bytes`

### E2E 字段

- `payload_backend`
- `payload_fetch_ms`
- `bytes_read`
- `fetch_count`
- `ann_core_ms`
- `overlap_ratio`
- `e2e_ms`

## 7. 建议的正式方法矩阵

### 主表矩阵

| 维度 | 选择 |
|------|------|
| 数据集 | COCO 100K / MS MARCO / Deep8M-synth / ESCI |
| 搜索核 | BoundFetch / IVFPQ+rerank / IVF+RaBitQ+rerank |
| 存储后端 | 1 个主后端 |
| 参考方法 | DiskANN 单列或单图 |

### 附录矩阵

| 维度 | 选择 |
|------|------|
| 数据集 | LAION subset / Clotho / MSR-VTT |
| 搜索核 | ConANN / DiskANN / 其他可选扩展 |
| 存储后端 | FlatStor / Lance / Parquet |

## 8. 当前建议的最小可发表 baseline 套餐

如果你现在要收敛到一套最小且足够强的正式 baseline，我建议是：

1. BoundFetch
2. Faiss IVFPQ + rerank
3. IVF + RaBitQ + rerank
4. DiskANN

这套组合对应当前正式论文叙事：`IVF+PQ`、`IVF+RaBitQ` 与强参考图索引三条主线。
