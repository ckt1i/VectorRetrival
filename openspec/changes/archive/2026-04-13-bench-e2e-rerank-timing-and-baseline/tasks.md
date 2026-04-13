## 1. Step 1：rerank_cpu_ms 计时拆分

- [x] 1.1 在 `include/vdb/query/search_context.h` 的 `SearchStats` 中添加 `double rerank_cpu_ms = 0;` 字段
- [x] 1.2 在 `src/query/overlap_scheduler.cpp` 的 `DispatchCompletion()` 中，对 `VEC_ONLY` 分支的 `reranker.ConsumeVec()` 加 `steady_clock` 计时，累加到 `ctx.stats().rerank_cpu_ms`
- [x] 1.3 同上，对 `VEC_ALL` 分支的 `reranker.ConsumeAll()` 加相同计时
- [x] 1.4 在 `benchmarks/bench_e2e.cpp` 的 `QueryMetrics` 中添加 `double avg_rerank_cpu = 0;` 字段
- [x] 1.5 在 `bench_e2e.cpp` 的结果收集处，从 `results.stats().rerank_cpu_ms` 读取并存入 `qr`；在 `ComputeMetrics()` 中累加计算均值
- [x] 1.6 修改控制台输出行格式为：`io_wait=X.XXX ms  cpu=X.XXX ms  probe=X.XXX ms  rerank_cpu=X.XXX ms`
- [x] 1.7 修改 HOT/COLD 对比表，新增 `rerank_cpu (ms)` 行
- [x] 1.8 修改 JSON 输出，在 metrics 对象中添加 `"avg_rerank_cpu_ms"` 字段
- [x] 1.9 重新编译：`cd build-bench-nomkl && cmake --build . --target bench_e2e -j8`
- [x] 1.10 在 COCO 100K 上运行验证，确认 `rerank_cpu_ms` 输出非零（预期约 1ms），且 `probe_ms + rerank_cpu_ms ≤ cpu_ms`

## 2. 环境准备与目录创建

- [x] 2.1 确认 `/home/zcq/VDB/baselines/` 目录存在（已有），创建子目录：`mkdir -p /home/zcq/VDB/baselines/{vector_search,payload_retrieval,e2e,data,results}`
- [x] 2.2 创建 `requirements.txt`：`faiss-cpu>=1.7, lancedb>=0.3, pyarrow>=12.0, numpy, diskannpy（可选）`
- [x] 2.3 安装依赖：`pip install faiss-cpu lancedb pyarrow numpy`（使用 conda lab env）
- [x] 2.4 尝试安装 DiskANN：`pip install diskannpy`；diskannpy 0.4.0 已安装于 lab env

## 3. 数据格式构建

- [x] 3.1 创建 `/home/zcq/VDB/baselines/payload_retrieval/build_datasets.py`，实现参数 `--dataset`（deep1m / coco_100k）、`--format`（lance / parquet / flatstor / all）、`--data-dir`
- [x] 3.2 实现 Deep1M Lance 数据集构建：读取 `base.fvecs` + 生成 10KB 合成 payload，用 `lancedb.connect().create_table()` 写入 `baselines/data/deep1m_lance/`
- [x] 3.3 实现 Deep1M Parquet 构建：写入 `baselines/data/deep1m.parquet`，列：id(int64)、vector(list<float32>)、payload(binary)，按 id 排序
- [x] 3.4 实现 COCO 100K Lance 数据集构建：读取向量 + caption payload（从 metadata.jsonl），写入 `baselines/data/coco100k_lance/`
- [x] 3.5 实现 COCO 100K Parquet 构建：写入 `baselines/data/coco100k.parquet`
- [x] 3.6 实现 FlatStor 模拟索引构建：从 BoundFetch `.dat` 文件提取 ID→(offset,size) 映射，保存为 numpy 数组 `baselines/data/{dataset}_flatstor.idx.npy`
- [x] 3.7 运行构建脚本：所有格式构建成功（deep1m lance/parquet/flatstor + coco_100k lance/parquet/flatstor）

## 4. Layer 1：向量搜索 Baseline（B1 FAISS IVF-PQ）

- [x] 4.1 创建 `/home/zcq/VDB/baselines/vector_search/run_faiss_ivfpq.py`，实现参数 `--dataset`、`--nlist`、`--nprobe`（逗号分隔）、`--topk`、`--save-ids`、`--output-csv`
- [x] 4.2 实现：加载 Deep1M fvecs + groundtruth，训练 IVF-PQ 索引（mmap），搜索，计算 recall@1/5/10，记录 per-query 延迟（p50/p99），结果追加到 `results/vector_search.csv`
- [x] 4.3 实现 `--save-ids` 选项：将最后一个 nprobe 的 Top-K IDs 保存为 `results/faiss_topk_ids_{dataset}_nprobe{N}.npy`（同时保存 faiss_topk_fidx 供 payload bench 使用）
- [x] 4.4 扩展脚本支持 COCO 100K（加载 `data/coco_100k/` 数据和 groundtruth）；新增 `--index-type ivfpq/ivfflat` 参数
- [x] 4.5 运行 Deep1M B1：`python run_faiss_ivfpq.py --dataset deep1m --nlist 4096 --nprobe 32,64,128,256 --topk 10 --save-ids`；结果：recall@10=1.0000 at all nprobe；avg=0.21/0.37/0.70/1.33ms
- [x] 4.6 运行 COCO 100K B1（IVF-Flat，因 IVF-PQ recall上限仅0.75）：`--nlist 2048 --index-type ivfflat --nprobe 32,64,128,200,300,500 --topk 10`；nprobe=32 recall@10=0.989 avg=0.155ms；nprobe=128 recall@10=1.000 avg=0.388ms；saved fidx nprobe=300

## 5. Layer 1：向量搜索 Baseline（B2 DiskANN，Optional）

- [x] 5.1 创建 `/home/zcq/VDB/baselines/vector_search/run_diskann.py`，若 `diskannpy` 不可用则打印安装说明并退出
- [x] 5.2 实现 Deep1M 索引构建（R=64，L=100，存储于 `baselines/data/deep1m_diskann/`）
- [x] 5.3 DiskANN 索引构建完成（1M pts/118s）；diskannpy 0.4.0 load_index 与 _mem.index 不兼容，已在 CSV 中标注 diskann_unavailable
- [x] 5.4 若 DiskANN 安装失败，在 CSV 中写入一行标注 `"diskann_unavailable"`，继续后续任务

## 6. Layer 2：Payload 检索 Baseline（B3 FlatStor + B4 Lance + B5 Parquet）

- [x] 6.1 创建 `/home/zcq/VDB/baselines/payload_retrieval/run_payload_bench.py`，实现参数 `--ids`（npy 文件路径）、`--backend`（flatstor / lance / parquet / all）、`--dataset`、`--topk`、`--output-csv`
- [x] 6.2 实现 FlatStor backend：加载 `{dataset}_flatstor.npy` offset 索引，对 Top-K IDs 做 `numpy` 数组索引取 offset，然后 `os.pread(dat_fd, size, offset)` 批量读取，记录延迟（仅 I/O，不含向量计算）
- [x] 6.3 实现 Lance backend：`lance.dataset(path).take(indices, columns=["payload"])`，记录 `take()` 端到端延迟
- [x] 6.4 实现 Parquet backend：`pyarrow.parquet.read_table(path, filters=[("id","in",ids)])` 或 pandas，记录延迟
- [x] 6.5 运行 Deep1M payload 检索（faiss_topk_fidx_deep1m_nprobe256.npy）：FlatStor avg=0.034ms；Lance avg=0.236ms；Parquet 跳过（10GB文件，filter扫描过慢）
- [x] 6.6 运行 COCO 100K payload 检索（faiss_topk_fidx_coco_100k_nprobe300.npy）：FlatStor avg=0.026ms；Lance avg=0.185ms；Parquet avg=7.524ms

## 7. E2E 汇总与对比

- [x] 7.1 创建 `/home/zcq/VDB/baselines/e2e/run_e2e_comparison.py`，读取 `results/vector_search.csv` + `results/payload_retrieval.csv`，计算各组合的 E2E latency
- [x] 7.2 实现 `--boundfetch-result` 参数，支持新旧两种 JSON 格式（metrics+pipeline_stats 分离或 inline）
- [x] 7.3 运行汇总：`python run_e2e_comparison.py --boundfetch-result .../results.json`，生成 `results/e2e_comparison.csv`（71 rows）
- [x] 7.4 写入 `results/analysis.md`：向量搜索对比表、Payload 检索对比表、E2E 组合表、BoundFetch 优势分析、FlatStor 近似误差说明
- [x] 7.5 更新 `refine-logs/EXPERIMENT_TRACKER_CN.md` 中第 0 阶段 B1-B5 任务状态为完成
