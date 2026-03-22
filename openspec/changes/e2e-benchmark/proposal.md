# Proposal: E2E Benchmark

## What

构建端到端集成测试 benchmark，使用真实 COCO 数据集 (1k/5k) 完成从数据加载、索引构建、查询到指标评估的完整流程。

## Why

- 当前所有测试均为合成数据的单元测试，缺乏真实数据的端到端验证
- 需要量化指标：recall@K、query latency、build time、pipeline IO/CPU 利用率
- 需要验证 PayloadFn、Early Stop、IoUring Pipeline 在真实数据下的正确性和性能

## Scope

### In Scope
- 加载 COCO 数据集 (NpyReader + JsonlReader)
- Brute-force L2 ground truth 计算 (recall@K, K>1)
- IvfBuilder 构建索引 (带 payload: id + caption)
- IoUring + OverlapScheduler 查询
- SearchStats 计时增强 (io_wait_time_ms)
- OverlapScheduler 填充已有的 probe_time_ms / total_time_ms
- 结果输出到 /home/zcq/VDB/test/<dataset>_<timestamp>/
- JSON 格式的 config.json + results.json（手动拼接，无新依赖）

### Out of Scope
- PreadFallback 对比
- 多线程查询
- Web UI / 可视化

## Data

| 数据集 | 路径 | N | Q | dim |
|--------|------|---|---|-----|
| coco_1k | /home/zcq/VDB/data/coco_1k | 1000 | 1000 | 512 |
| coco_5k | /home/zcq/VDB/data/coco_5k | 5000 | 5000 | 512 |

数据集结构：
```
image_embeddings.npy   [N, 512] float32
image_ids.npy          [N] int64
query_embeddings.npy   [Q, 512] float32
query_ids.npy          [Q] int64
metadata.jsonl         {image_id, caption, image_path, ...}
queries.jsonl          {query_id, image_id, text}
```

Ground truth: query_id == image_id (值匹配)，仅提供 recall@1。recall@K (K>1) 需暴力搜索。

## Decisions

- Reader: 仅使用 IoUringReader
- nlist: 统一 32
- JSON: 手动拼字符串，不引入 nlohmann/json
- 抽样: per_query 详情抽样输出 (每 20 条取 1 条)
