# Spec: E2E Benchmark

## 功能规格

### 输入
- 数据集路径 (硬编码或命令行参数): `/home/zcq/VDB/data/coco_1k` 或 `coco_5k`
- 构建参数: nlist=32, nprobe=8, top_k=10

### 输出
- 索引文件: `<output_dir>/index/`
- config.json: 完整参数快照
- results.json: 指标 + 抽样 per_query

### 指标要求

| 指标 | 说明 | 来源 |
|------|------|------|
| recall@1 | ground truth id 匹配 + brute-force | 比较 predicted vs gt |
| recall@5 | brute-force top-5 交集 | 比较 predicted vs gt |
| recall@10 | brute-force top-10 交集 | 比较 predicted vs gt |
| training_time_ms | IvfBuilder::Build 耗时 | chrono wall clock |
| brute_force_time_ms | 暴力搜索 GT 耗时 | chrono wall clock |
| avg_query_time_ms | 全量查询平均延迟 | chrono per-query |
| p50/p95/p99_query_time_ms | 查询延迟分位数 | 排序后取分位 |
| io_wait_time_ms | WaitAndPoll 累计等待 | SearchStats |
| cpu_time_ms | total - io_wait | 计算得出 |
| probe_time_ms | ProbeCluster 累计 | SearchStats |
| early_stopped_pct | 触发 early stop 的查询比例 | SearchStats |
| avg_clusters_skipped | 平均跳过的聚类数 | SearchStats |

### 目录命名
格式: `{dataset_name}_{timestamp}`
- dataset_name: `coco_1k` 或 `coco_5k`
- timestamp: `%Y%m%dT%H%M%S` (本地时间)
- 示例: `coco_1k_20260321T143000`

### per_query 抽样
- 抽样率: 每 20 条取 1 条 (sample_stride = Q / 50, 至少 1)
- 包含: query_id, gt_top10, predicted_top10, distances, recall, timing, early_stop

### 非功能要求
- 单线程执行
- 无新外部依赖 (JSON 手动拼接)
- 使用 IoUringReader
