# Design: E2E Benchmark

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        bench_e2e.cpp                                    │
└─────────────────────────────────────────────────────────────────────────┘

  Phase A: Load ──→ Phase B: BruteForce GT ──→ Phase C: Build ──→ Phase D: Query ──→ Phase E: Output
```

## Phase A: Data Loading

```cpp
// NpyReader 加载向量和 ID
auto images = LoadNpyFloat32(data_dir + "/image_embeddings.npy");    // [N, 512]
auto img_ids = LoadNpyInt64(data_dir + "/image_ids.npy");            // [N]
auto queries = LoadNpyFloat32(data_dir + "/query_embeddings.npy");   // [Q, 512]
auto qry_ids = LoadNpyInt64(data_dir + "/query_ids.npy");            // [Q]

// JsonlReader 构建 metadata map
std::unordered_map<int64_t, std::string> id_to_caption;
ReadJsonlLines(data_dir + "/metadata.jsonl", [&](uint32_t, string_view line) {
    // 手动解析 JSON: 提取 image_id 和 caption
});
```

### metadata.jsonl 解析

无 JSON 库依赖。字段提取策略：
- `image_id`: 匹配 `"image_id": <number>` 正则
- `caption`: 匹配 `"caption": "<string>"` (找第一个 caption 字段)

只需两个字段，手动子串查找即可。

## Phase B: Brute-Force Ground Truth

```
for each query q_i (i = 0..Q-1):
    for each image j (j = 0..N-1):
        dist[j] = L2Sqr(q_vec[i], img_vec[j], dim)
    partial_sort(dist, top_k) → gt_topk[i] = {image_id_1, ..., image_id_K}
```

- 使用现有 `vdb::L2Sqr` 函数
- `std::partial_sort` 取 top-K，无需全排序
- gt_topk 存储 image_id (int64)，不存储 distance

## Phase C: Build Index

```cpp
IvfBuilderConfig cfg;
cfg.nlist = 32;
cfg.max_iterations = 20;
cfg.seed = 42;
cfg.rabitq = {1, 64, 5.75f};
cfg.calibration_samples = std::min(100u, N);
cfg.calibration_topk = 10;
cfg.page_size = 4096;
cfg.payload_schemas = {
    {0, "id",      DType::INT64,  false},
    {1, "caption", DType::STRING, false},
};

// image_ids[idx] → 从 id_to_caption map 取 caption
PayloadFn payload_fn = [&](uint32_t idx) -> std::vector<Datum> {
    int64_t id = img_ids.data[idx];
    auto it = id_to_caption.find(id);
    std::string cap = (it != id_to_caption.end()) ? it->second : "";
    return {Datum::Int64(id), Datum::String(std::move(cap))};
};

// ⏱ training_time 计时范围
auto t0 = steady_clock::now();
builder.Build(images.data.data(), N, dim, index_dir, payload_fn);
auto training_time_ms = duration<double,milli>(steady_clock::now() - t0).count();
```

## Phase D: Query + Timing

```cpp
IoUringReader reader;
SearchConfig search_cfg;
search_cfg.top_k = 10;
search_cfg.nprobe = 8;
search_cfg.early_stop = true;

OverlapScheduler scheduler(index, reader, search_cfg);

for each query q_i:
    auto t0 = steady_clock::now();
    auto results = scheduler.Search(q_vec);
    auto query_time = duration<double,milli>(steady_clock::now() - t0).count();

    // results.stats() 包含:
    //   total_time_ms      (Search 内部 wall clock)
    //   io_wait_time_ms    (WaitAndPoll 累计)
    //   probe_time_ms      (ProbeCluster 累计)
    //   early_stopped, clusters_skipped
```

### SearchStats 计时增强

在 `SearchStats` 中添加：
```cpp
double io_wait_time_ms = 0;   // 新增: Σ WaitAndPoll 耗时
```

在 `OverlapScheduler::Search()` 中：
```cpp
SearchResults Search(const float* query_vec) {
    auto t_start = steady_clock::now();
    // ... existing flow ...
    auto t_end = steady_clock::now();
    sr.stats().total_time_ms = duration<double,milli>(t_end - t_start).count();
    return sr;
}
```

在每个 `WaitAndPoll` 调用处：
```cpp
auto tw0 = steady_clock::now();
uint32_t n = reader_.WaitAndPoll(...);
auto tw1 = steady_clock::now();
ctx.stats().io_wait_time_ms += duration<double,milli>(tw1 - tw0).count();
```

在每个 `ProbeCluster` 调用处：
```cpp
auto tp0 = steady_clock::now();
ProbeCluster(ready_clusters_[cid], cid, ctx, reranker);
auto tp1 = steady_clock::now();
ctx.stats().probe_time_ms += duration<double,milli>(tp1 - tp0).count();
```

## Phase E: Recall Computation

```
recall@K = (1/Q) × Σ_i |predicted_topK[i] ∩ gt_topK[i]| / K
```

对每个查询：
- 从 SearchResults 提取 predicted_ids (payload[0].i64)
- 与 gt_topk[i] 做集合交集
- 计算 recall@1, recall@5, recall@10

## Phase F: Output

### 目录结构

```
/home/zcq/VDB/test/coco_1k_20260321T143000/
├── index/
│   ├── segment.meta
│   ├── cluster.clu
│   └── data.dat
├── config.json
└── results.json
```

目录名格式: `{dataset}_{timestamp}` 其中 timestamp = `%Y%m%dT%H%M%S`

### config.json (手动拼 JSON 字符串)

```json
{
  "dataset": "coco_1k",
  "dataset_path": "/home/zcq/VDB/data/coco_1k",
  "timestamp": "2026-03-21T14:30:00",
  "num_images": 1000,
  "num_queries": 1000,
  "dimension": 512,
  "build_config": {
    "nlist": 32,
    "max_iterations": 20,
    "seed": 42,
    "rabitq_sub_dim": 1,
    "rabitq_bits": 64,
    "rabitq_k_factor": 5.75,
    "page_size": 4096
  },
  "search_config": {
    "top_k": 10,
    "nprobe": 8,
    "early_stop": true,
    "prefetch_depth": 16,
    "io_queue_depth": 64
  }
}
```

### results.json

```json
{
  "metrics": {
    "training_time_ms": 123.4,
    "brute_force_time_ms": 456.7,
    "recall_at_1": 0.85,
    "recall_at_5": 0.95,
    "recall_at_10": 0.98,
    "avg_query_time_ms": 1.23,
    "p50_query_time_ms": 1.10,
    "p95_query_time_ms": 2.80,
    "p99_query_time_ms": 3.45,
    "num_queries": 1000
  },
  "pipeline_stats": {
    "avg_total_probed": 800,
    "avg_safe_in": 200,
    "avg_safe_out": 500,
    "avg_uncertain": 100,
    "avg_io_wait_ms": 0.5,
    "avg_cpu_time_ms": 0.73,
    "avg_probe_time_ms": 0.6,
    "early_stopped_pct": 0.6,
    "avg_clusters_skipped": 3.2
  },
  "per_query_sample": [
    {
      "query_id": 139,
      "gt_top10_ids": [139, 285, ...],
      "predicted_top10_ids": [139, 285, ...],
      "predicted_top10_distances": [0.0, 1.23, ...],
      "hit_at_1": true,
      "recall_at_10": 0.9,
      "query_time_ms": 1.1,
      "io_wait_ms": 0.3,
      "cpu_time_ms": 0.8,
      "early_stopped": true,
      "clusters_skipped": 4
    }
  ]
}
```

## JSON 手动拼接策略

编写简单的辅助函数：

```cpp
std::string JsonString(const std::string& key, const std::string& val);  // "key": "val"
std::string JsonNumber(const std::string& key, double val);              // "key": 1.23
std::string JsonBool(const std::string& key, bool val);                  // "key": true
std::string JsonArray(const std::string& key, const std::vector<int64_t>& vals);
std::string JsonArrayF(const std::string& key, const std::vector<float>& vals);
```

在 bench_e2e.cpp 内部定义，不对外暴露。

## IoUring 初始化

```cpp
IoUringReader reader;    // 默认构造即可
// IoUringReader::Init() 在首次 PrepRead 时自动调用
// 如果 io_uring 不可用会 fallback（但我们这里假定可用）
```

需要验证 IoUringReader 的构造/初始化接口。
