# Spec: ConANN Recall Benchmark

## 功能描述

使用精确 L2 距离（不使用 RaBitQ）+ ConANN d_k early stop 评估 IVF 分区的 recall 上限。

## 输入

### CLI 参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--dataset` | string | `/home/zcq/VDB/data/coco_1k` | 数据集路径 |
| `--nlist` | int | 16 | K-Means 聚类数 |
| `--nprobe` | int | 8 | 最大探查聚类数 |
| `--topk` | int | 10 | Top-K |
| `--p-for-dk` | int | 99 | d_k 校准百分位 |
| `--queries` | int | 0 | query 数量限制 (0=all) |
| `--max-iter` | int | 20 | K-Means 迭代数 |
| `--seed` | int | 42 | 随机种子 |

### 数据集

- `image_embeddings.npy`: database 向量 (N × dim, float32)
- `query_embeddings.npy`: query 向量 (Q × dim, float32)

## 处理流程

1. 加载数据集
2. 暴力搜索计算 ground truth top-K（per query）
3. K-Means 聚类 database 向量
4. 校准 d_k: `ConANN::CalibrateDistanceThreshold(queries, Q, database, N, dim, ...)`（query/database 分离校准）
5. 对每个 query:
   a. 计算 query 到所有 centroid 的 L2² 距离，取最近 nprobe 个
   b. 按 centroid 距离从近到远逐个 probe 聚类
   c. 对聚类内所有向量计算精确 L2²，更新 top-k max-heap
   d. Early stop: top-k 填满且 heap.top() < d_k 时跳过剩余聚类
6. 计算 recall 和统计指标

## 输出

### 汇总指标（stdout）

| 指标 | 说明 |
|------|------|
| recall@1 | 平均 top-1 命中率 |
| recall@5 | 平均 top-5 recall |
| recall@10 | 平均 top-10 recall |
| avg_probes | 平均实际 probe 聚类数 |
| early_stop_rate | 触发 early stop 的 query 比例 |
| d_k | 校准得到的距离阈值 |

### per-query 采样

每 20 条 query 输出 1 条详情：
- query index, recall@1/5/10, clusters_probed, early_stopped, worst_topk_dist

## 非功能要求

- 单线程 CPU 执行
- 不使用 RaBitQ（精确 L2 距离）
- 不依赖磁盘 I/O（全内存计算）
- 无新外部依赖
