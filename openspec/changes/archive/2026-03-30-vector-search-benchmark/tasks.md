# 任务：纯向量搜索 Benchmark

## 1. fvecs/ivecs IO 库

- [x] 1.1 新增 `include/vdb/io/vecs_reader.h`：声明 `LoadFvecs(path) → StatusOr<NpyArrayFloat>`、`VecsArrayInt32` 类型、`LoadIvecs(path) → StatusOr<VecsArrayInt32>`
- [x] 1.2 新增 `src/io/vecs_reader.cpp`：实现 fvecs/ivecs 读取（读取首条记录的 dim，校验后续记录 dim 一致，一次性加载）
- [x] 1.3 在 CMakeLists.txt 的 `vdb_io` 库中添加 `vecs_reader.cpp`
- [x] 1.4 新增 `tests/io/vecs_reader_test.cpp`：用小型 fvecs/ivecs 文件测试读取正确性

## 2. bench_vector_search 基本框架

- [x] 2.1 新增 `benchmarks/bench_vector_search.cpp`：CLI 参数解析（--base, --query, --gt, --nlist, --nprobe, --topk 等）
- [x] 2.2 Phase A：数据加载 — 按扩展名（.fvecs/.npy）自动选择 reader，加载 base、query、GT（可选）
- [x] 2.3 Phase B：Build — KMeans 聚类 + RaBitQ 编码 + ε_ip/d_k 校准（复用 bench_rabitq_diagnostic 中的 KMeans + 编码逻辑）

## 3. 向量搜索核心

- [x] 3.1 Phase D：Probe 循环 — 对每个 query，按质心距排序后逐 cluster probe：RaBitQ 估计 → ConANN 分类 → rerank（精确 L2）。维护 est_heap（估计距离）+ exact_heap（精确距离）
- [x] 3.2 Legacy early-stop：exact_heap.full && exact_heap.top < d_k → break
- [x] 3.3 GT 处理：有 .ivecs 则加载，无则暴力 L2 自算
- [x] 3.4 Phase E：Recall 计算（@1, @5, @10, @100）+ 延迟统计（avg/p50/p95/p99）

## 4. CRC Early-Stop 接入

- [x] 4.1 Phase C：CRC 内联标定 — `CalibrateWithRaBitQ(config, queries, centroids, clusters, rotation)` → CalibrationResults → CrcStopper
- [x] 4.2 在 probe 循环中接入 CRC：`est_kth = est_heap.top()`，`crc_stopper.ShouldStop(probed_count, est_kth)` → break
- [x] 4.3 CRC 统计输出：avg_probed、early_stop_rate、FNR

## 5. 现有 Benchmark 适配 fvecs

- [x] 5.1 改造 `bench_rabitq_accuracy.cpp`：新增 `--base`/`--query` CLI 参数，数据加载改用 `LoadVectors()`，保留 `--dataset` 回退（未指定 --base 时默认 `{dataset}/image_embeddings.npy`）
- [x] 5.2 改造 `bench_rabitq_diagnostic.cpp`：同上，新增 `--base`/`--query` + `LoadVectors()` + `--dataset` 回退

## 6. 输出与构建

- [x] 6.1 Phase F：终端输出 + JSON 结果文件（--outdir）
- [x] 6.2 CMakeLists.txt：新增 `bench_vector_search` target，link `vdb_crc vdb_index vdb_io`
- [x] 6.3 构建验证：`cmake --build build`，确保所有 target 通过

## 7. 验证

- [x] 7.1 在 DEEP1M 上运行 `bench_vector_search --base .../deep1m_base.fvecs --query .../deep1m_query.fvecs --gt .../deep1m_groundtruth.ivecs --nlist 256 --nprobe 32 --topk 10`，验证 recall — recall@1=1.0, recall@10=0.987
- [x] 7.2 在 DEEP1M 上加 `--crc 1`，验证 CRC early-stop 触发、λ̂ < 1.0 — lamhat=0.186, early_stop_rate=0.68, recall@10=0.989
- [x] 7.3 在 COCO_5k 上运行（.npy 格式），验证格式探测正确、recall 合理 — recall@10=0.926, brute-force GT
- [x] 7.4 在 DEEP1M 上运行 `bench_rabitq_accuracy --base .../deep1m_base.fvecs --query .../deep1m_query.fvecs`，验证 fvecs 加载正确 — N=1000000, dim=96, loaded successfully
