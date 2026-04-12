## 1. 环境与数据准备

- [x] 1.1 切换到 `labnew` conda env，验证 Python 版本 == 3.11.x：`/home/zcq/anaconda3/envs/labnew/bin/python --version`
- [x] 1.2 在 `labnew` 中安装依赖：`pip install numpy==1.25 setuptools>=59.6 pybind11>=2.10.0 cmake>=3.22 faiss-cpu==1.9.0 diskannpy==0.6.0 lancedb pyarrow pandas`（注：diskannpy 0.7.0 在此内核上 StaticDiskIndex 有 libaio futex 死锁，0.6.0 正常工作；faiss-cpu 需 ==1.9.0，更新版本与 numpy==1.25 不兼容）
- [x] 1.3 创建 `/home/zcq/VDB/baselines/check_labnew.py`，实现对 faiss/diskannpy/lancedb/pyarrow 版本校验，提供 exit code 0/1
- [x] 1.4 运行 `check_labnew.py`，确认全部依赖通过
- [x] 1.5 快速验证 diskannpy 0.6.0 可用：用 10K toy 数据集 `build_disk_index` → `StaticDiskIndex.search` 能返回结果（注：0.6.0 的 search() 返回 tuple，0.7.0 返回 QueryResponse 对象）
- [x] 1.6 diskannpy 0.7.0 StaticDiskIndex 在本机内核 6.8.0 上挂起（libaio + O_DIRECT futex 死锁）；改用 diskannpy==0.6.0，无需 C++ binary 回退
- [x] 1.7 创建 `/home/zcq/VDB/baselines/_cache_utils.py`：只提供 `drop_file_cache(path)` 使用 `posix_fadvise(DONTNEED)`（不提供 sudo 版本，拒绝任何 `--use-sudo` 调用）
- [x] 1.8 单元测试 `_cache_utils.py`：对一个 10MB 文件调用 `drop_file_cache`，通过 `/proc/meminfo` 的 Cached 字段变化观察是否生效（结果：delta=1512 KB，kernel 部分响应了 DONTNEED 提示；仅信息性）

## 2. 数据集就绪

- [x] 2.1 验证 Deep8M 数据存在且可读：`/home/zcq/VDB/data/deep8m/deep8m_base.fvecs`（2.9GB）、`deep8m_query.fvecs`（100K queries）、`deep8m_groundtruth.ivecs`（top-1000 GT for 100K queries）
- [x] 2.2 新建 `/home/zcq/VDB/baselines/_dataset_loaders.py`，提供 `load_dataset(name, max_queries, data_dir, results_dir)` 统一接口，含 deep1m / deep8m / coco_100k 三个 loader
- [x] 2.3 实现 Deep8M 1000-query 采样：均匀采样 1000 个 query，索引缓存到 `results/deep8m_query1000_idx.npy`
- [x] 2.4 预计算 COCO 100K groundtruth@10：一次性 brute-force（119.5s），保存到 `/home/zcq/VDB/data/coco_100k/groundtruth_top10.npy`

## 3. CSV schema 迁移（v4 → v5）

- [x] 3.1 创建 `/home/zcq/VDB/baselines/migrate_v4_rows.py`：读 `results/vector_search.csv` 和 `payload_retrieval.csv`，新增 `protocol` 列（旧行默认 `memory`），给旧 FAISS 行的 `notes` 追加 `in-memory-reference-only`
- [x] 3.2 运行 migration，验证 CSV 格式正确、旧数据完整（16 rows vector_search，12 rows payload_retrieval，全部 PASS）
- [x] 3.3 备份 v4 原始 CSV 到 `results/v4_backup/`（vector_search_v4.csv + payload_retrieval_v4.csv）

## 4. B1：FAISS OnDiskInvertedLists baseline

- [x] 4.1 创建 `/home/zcq/VDB/baselines/vector_search/run_faiss_ivfpq_disk.py`，实现参数 `--dataset`、`--nlist`、`--nprobe`、`--topk`、`--protocol warm/semi_cold`（默认 warm）、`--save-ids`、`--query-count`（默认 1000）；拒绝 `--use-sudo`
- [x] 4.2 实现 OnDiskInvertedLists 索引构建：`IndexIVFPQ → train → replace_invlists(OnDiskInvertedLists) → add → write_index`；缓存到 `results/faiss_ivfpq_disk_{dataset}_nlist{N}.index` + `.ondisk`
- [x] 4.3 实现 WARM 协议：100 warmup query（丢弃延迟）+ 1000 measurement query（可通过 `--warmup-count`、`--query-count` 调整）
- [x] 4.4 实现 semi_cold 协议：每个 nprobe 循环前 `drop_file_cache(stub_path + ondisk_path)`，然后走 warmup+measurement；记录 protocol=semi_cold
- [x] 4.5 实现 recall 计算 + 延迟 avg/p50/p99 记录
- [x] 4.6 实现 `--save-ids`：保存 `results/faiss_disk_topk_fidx_{dataset}_nprobe{N}.npy` 和（COCO 100K）`faiss_disk_topk_ids_{dataset}_nprobe{N}.npy`
- [x] 4.7 追加结果到 `results/vector_search.csv`（系统名 `FAISS-IVFPQ-disk`，含 `protocol` 列）
- [x] 4.8 运行 Deep1M WARM：`nlist=4096, nprobe=32,64,128,256` - recall@10=1.0000, avg=1.36~3.22ms
- [x] 4.9 运行 Deep8M WARM：`nlist=12800, nprobe=32,64,128,256` - recall@10=1.0000, recall@1=0.919~0.928, avg=1.70~5.23ms
- [x] 4.10 运行 COCO 100K WARM：`nlist=2048, nprobe=32,64,128,300` - recall@10=0.742~0.747（PQ压缩CLIP低精度，已知问题）
- [x] 4.11 （已跳过）semi_cold 实验因无 sudo 权限暂不跑
- [x] 4.12 成功判据：Deep8M nprobe=128 recall@10=1.000 ≥ 0.90 ✓；WARM 延迟数据完整 ✓

## 5. B2：DiskANN disk-mode baseline

- [x] 5.1 创建 `/home/zcq/VDB/baselines/vector_search/run_diskann_disk.py`，实现参数 `--dataset`、`--R`、`--L`（build complexity）、`--L_search`、`--beam`、`--topk`、`--protocol warm/semi_cold`（默认 warm）、`--save-ids`；拒绝 `--use-sudo`；兼容 0.6.0 的 tuple 返回格式
- [x] 5.2 实现 DiskANN disk index 构建：`diskannpy.build_disk_index(...)` with `search_memory_maximum=1.0`、`pq_disk_bytes=D`（`--pq-disk-bytes 0` 自动等于 D）
- [x] 5.3 验证构建产物：`{prefix}_disk.index` 非空、`{prefix}_pq_compressed.bin` 存在；否则 exit 2
- [x] 5.4 实现索引加载：`StaticDiskIndex(index_directory, num_threads=1, num_nodes_to_cache=N*nodes_cache_frac)`（默认 N/10）
- [x] 5.5 实现 WARM 协议：100 warmup query + 1000 measurement query
- [x] 5.6 实现 semi_cold 协议：load 前 `drop_file_cache` on `{prefix}_disk.index` + `{prefix}_pq_compressed.bin`
- [x] 5.7 实现 per-query 搜索 + 延迟/recall 记录（per-query 使用 `time.perf_counter`）
- [x] 5.8 实现 `--save-ids`：`results/diskann_topk_fidx_{dataset}_L{N}.npy` 和（COCO）entity_id 版本
- [x] 5.9 追加到 `results/vector_search.csv`（系统名 `DiskANN-disk`）
- [x] 5.10 失败回退：若 `diskannpy` import 失败或 build 产物缺失，打印错误并 exit 2；不写假数据
- [x] 5.11 运行 Deep1M WARM：`R=64 L=100 L_search={50,100,200}` - recall@10=1.0000, avg=2.45~8.81ms
- [x] 5.12 运行 Deep8M WARM：`R=64 L=100 L_search={50,100,200,300}` - recall@10=1.0000, recall@1=0.987~0.989, avg=2.70~13.84ms
- [x] 5.13 运行 COCO 100K WARM：`R=64 L=100 L_search={50,100,200}` - recall@10=1.0000, recall@1=0.977~0.986, avg=3.77~13.20ms
- [x] 5.14 （已跳过）semi_cold sanity check 因无 sudo 权限暂不跑
- [x] 5.15 成功判据：三个数据集能跑通 WARM；Deep8M L_search=200 recall@10 ≥ 0.90

## 6. B5 扩展：Deep8M payload + COCO 100K COLD 补测

- [x] 6.1 扩展 `build_datasets.py` 支持 Deep8M：Lance/Parquet payload=int64 entity_id（8B），FlatStor 8M×16B idx
- [x] 6.2 运行 Deep8M 数据构建：flatstor(0.2s), parquet(57s), lance(59s)
- [x] 6.3 扩展 `run_payload_bench.py` 支持 `--protocol warm/semi_cold`（默认 warm）
- [x] 6.4 扩展 `run_payload_bench.py` 路径映射：支持 `deep8m` dataset key
- [x] 6.5 Deep8M payload WARM：FlatStor=0.015ms, Lance=4.69ms, Parquet=15.76ms
- [x] 6.6 COCO 100K payload WARM：FlatStor=0.022ms, Lance=6.96ms, Parquet=12.19ms
- [x] 6.7 结果已追加到 `results/payload_retrieval.csv`

## 7. BoundFetch 参考 E2E 重跑（COCO 100K）

- [x] 7.1 编译 bench_e2e：修复 --index-dir 时 output_dir 未创建的 bug，重新编译成功
- [x] 7.2 运行 bench_e2e on COCO 100K WARM：nprobe=200, queries=1000，输出到 /tmp/bench_bf_warm/coco_100k_20260412T142425/
- [x] 7.3 BoundFetch 结果：avg_query=5.42ms, p99=5.80ms, io_wait=0.003ms, cpu=5.42ms, probe=4.22ms, recall@10=0.964
- [x] 7.4 JSON 包含 avg_rerank_cpu_ms=0.015ms ✓

## 8. Layer 3：E2E comparison

- [x] 8.1 扩展 `run_e2e_comparison.py` 支持 `--protocol warm/semi_cold/all` 过滤（默认 warm）
- [x] 8.2 扩展 filter 逻辑：join key = (dataset, topk, protocol)
- [x] 8.3 扩展 BoundFetch 参考行：提取 avg_io_wait_ms/avg_cpu_ms/avg_rerank_cpu_ms 写入 notes
- [x] 8.4 运行 E2E WARM 汇总：coco_100k，22 行结果生成
- [x] 8.5 生成 `results/e2e_comparison_warm.csv` ✓

## 9. 分析与输出

- [x] 9.1 更新 `analysis.md`：添加 v5 Sections 7-11，含 WARM 向量搜索、payload、BoundFetch、E2E 对比、决策门
- [x] 9.2 决策门：BoundFetch WARM E2E=5.42ms > DiskANN+FlatStor=3.79ms；WARM 下论文主论点不成立，建议补 COLD 实验或调整论文论证路径
- [ ] 9.3 更新 `refine-logs/EXPERIMENT_TRACKER_CN.md`（optional）
- [ ] 9.4 更新 `refine-logs/BASELINE_PLAN_CN.md` 添加 "v5 实际结果" 小节（optional）
- [ ] 9.5 生成汇总图（optional）

## 10. 清理与归档

- [ ] 10.1 删除/移动临时文件（Deep8M build 过程中的中间文件、失败的 diskann 尝试）
- [ ] 10.2 在 `/home/zcq/VDB/baselines/README.md` 简短记录 v5 新增的脚本和数据路径
- [ ] 10.3 验证所有脚本的 shebang 使用 `#!/usr/bin/env python3`，不直接硬编码 labnew 路径（脚本内注释说明运行环境要求）
