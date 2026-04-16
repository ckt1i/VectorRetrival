# Baseline 方法方案

日期：2026-04-16

## 1. baseline 总览

正式 baseline 分成三类：

| 类别 | 方法 | 角色 |
|------|------|------|
| 主 baseline | Faiss IVFPQ + rerank | 最基础的 IVF+rerank 对照 |
| 主 baseline | Faiss IVFPQR | 更强的 PQ + residual refine 对照 |
| 主 baseline | Faiss IVFResidualQuantizer 或 IVFLocalSearchQuantizer | 更强的 IVF+RQ 家族对照 |
| 可选增强 | ConANN + Faiss IVF | 验证“自适应 cluster probing”这一类贡献 |
| 可选增强 | 外部 IVF + RaBitQ / Extended-RaBitQ | 验证“量化器本身”带来的收益 |
| 强参考 | DiskANN | 图索引上界参考与 build/startup 对照 |

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

### 2.2 Faiss IVFPQR

定义：

- IVF + PQ 编码
- residual refine

作用：

- 对标“更强的 IVF+PQ+refine”路线
- 判断 BoundFetch 的优势是否只是因为 baseline 量化器太弱

实现要求：

- 与 IVFPQ+rerank 使用尽量接近的 nlist / nprobe 网格
- 单独记录 refine 带来的延迟增量

### 2.3 Faiss IVFResidualQuantizer 或 IVFLocalSearchQuantizer

二选一即可，优先选工程更稳的一种。

作用：

- 补齐 `IVF + RQ` 这条正式 baseline 线
- 这是当前方案最需要补的一环

建议：

- 如果 residual quantizer 跑通稳定，优先它
- 如果 local search quantizer 更易集成，则用它替代

## 3. 可选增强 baseline

### 3.1 ConANN + Faiss IVF

定位：

- 验证“自适应 probing / conformal control”这类收益是否独立存在

适用场景：

- 当你想把 `cluster probing 控制` 这部分贡献单独拿出来解释时

是否主表必需：

- 否
- 建议作为增强 baseline 或机制表中的方法对照

### 3.2 外部 IVF + RaBitQ / Extended-RaBitQ

定位：

- 验证“RaBitQ 量化器本身”能带来多少收益

作用：

- 把“量化器优势”和“系统协同优势”拆开

是否主表必需：

- 否
- 如果复现顺利，强烈建议加进 appendix 或 secondary figure

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
| 搜索核 | BoundFetch / IVFPQ+rerank / IVFPQR / IVF-RQ |
| 存储后端 | 1 个主后端 |
| 参考方法 | DiskANN 单列或单图 |

### 附录矩阵

| 维度 | 选择 |
|------|------|
| 数据集 | LAION subset / Clotho / MSR-VTT |
| 搜索核 | ConANN / 外部 RaBitQ / DiskANN |
| 存储后端 | FlatStor / Lance / Parquet |

## 8. 当前建议的最小可发表 baseline 套餐

如果你现在要收敛到一套最小且足够强的正式 baseline，我建议是：

1. BoundFetch
2. Faiss IVFPQ + rerank
3. Faiss IVFPQR
4. Faiss IVFResidualQuantizer
5. DiskANN

这套组合已经明显强于目前“Faiss disk + DiskANN”两点式 baseline。
