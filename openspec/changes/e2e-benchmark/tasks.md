# Tasks: E2E Benchmark

## Task 1: SearchStats 计时增强
**Files**: `include/vdb/query/search_context.h`, `src/query/overlap_scheduler.cpp`
**Description**:
- SearchStats 添加 `double io_wait_time_ms = 0;`
- OverlapScheduler::Search() 添加 total_time_ms 计时 (wall clock)
- 每个 WaitAndPoll 调用前后加 chrono 计时，累加到 io_wait_time_ms
- 每个 ProbeCluster 调用前后加 chrono 计时，累加到 probe_time_ms
**Estimate**: ~30 行改动

## Task 2: bench_e2e.cpp 主体
**Files**: `tests/index/bench_e2e.cpp` (新建)
**Description**:
- main() 接受数据集路径参数 (默认 coco_1k)
- Phase A: NpyReader 加载 image_embeddings, image_ids, query_embeddings, query_ids
- Phase A: JsonlReader 加载 metadata.jsonl → map<image_id, caption>
- Phase B: Brute-force L2 ground truth (top-K for all queries)
- Phase C: IvfBuilder::Build 带 PayloadFn (id + caption)
- Phase D: IoUringReader + OverlapScheduler 查询全部 queries
- Phase E: Recall@1/5/10 计算、timing 聚合 (avg, p50, p95, p99)
- Phase F: 输出 config.json + results.json (手动拼 JSON)
- 创建输出目录: /home/zcq/VDB/test/{dataset}_{timestamp}/
**Estimate**: ~350 行
**Depends on**: Task 1

## Task 3: CMakeLists.txt 集成
**Files**: `CMakeLists.txt`
**Description**:
- 添加 bench_e2e target
- 链接: vdb_query vdb_io (NpyReader/JsonlReader)
**Estimate**: ~5 行
**Depends on**: Task 2

## Task 4: 运行验证
**Description**:
- 编译 bench_e2e
- 用 coco_1k 运行，验证输出目录和 JSON 文件正确
- 检查 recall 数值合理性 (recall@1 > 0.5, recall@10 > 0.8)
- 检查 pipeline stats (io_wait_time, probe_time 非零)
**Depends on**: Task 3
