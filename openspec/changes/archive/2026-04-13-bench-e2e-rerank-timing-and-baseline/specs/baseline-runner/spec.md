## ADDED Requirements

### Requirement: FAISS IVF-PQ 向量搜索脚本（B1）
系统 SHALL 在 `/home/zcq/VDB/baselines/vector_search/run_faiss_ivfpq.py` 提供 FAISS IVF-PQ baseline，在 Deep1M 和 COCO 100K 上输出 recall/latency 和 Top-K IDs。

#### Scenario: Deep1M FAISS IVF-PQ 搜索并保存 Top-K IDs
- **WHEN** 以 `--dataset deep1m --nlist 4096 --nprobe 64 --topk 10 --save-ids` 参数运行
- **THEN** 输出 `recall@10`、`avg_latency_ms`、`p99_latency_ms` 到 CSV，并将 Top-K IDs 保存为 `results/faiss_topk_ids_deep1m.npy`（shape: [Q, K]）

#### Scenario: COCO 100K FAISS IVF-PQ 搜索并保存 Top-K IDs
- **WHEN** 以 `--dataset coco_100k --nlist 2048 --nprobe 150 --topk 10 --save-ids` 参数运行
- **THEN** 输出 recall/latency CSV，并保存 Top-K IDs 到 `results/faiss_topk_ids_coco100k.npy`

#### Scenario: 多 nprobe 扫描
- **WHEN** 以 `--nprobe 32,64,128,150` 参数运行
- **THEN** 对每个 nprobe 值输出一行 CSV 结果；只在最后一个 nprobe 值时保存 Top-K IDs（或由参数 `--save-nprobe` 指定）

### Requirement: DiskANN 向量搜索脚本（B2，Optional）
系统 SHALL 在 `/home/zcq/VDB/baselines/vector_search/run_diskann.py` 提供 DiskANN baseline；若 `diskannpy` 不可用则优雅退出并提示安装命令。

#### Scenario: DiskANN Deep1M 搜索
- **WHEN** 以 `--dataset deep1m --R 64 --L 100 --L_search 100 --topk 10` 参数运行
- **THEN** 输出 `recall@10`、`avg_latency_ms`、`p99_latency_ms` 到 CSV，并保存 Top-K IDs

#### Scenario: diskannpy 不可用时优雅降级
- **WHEN** `diskannpy` 未安装
- **THEN** 打印明确错误提示和安装命令，以非零状态退出；不静默失败

### Requirement: 数据格式构建脚本
系统 SHALL 在 `/home/zcq/VDB/baselines/payload_retrieval/build_datasets.py` 提供一键构建脚本，将 Deep1M 和 COCO 100K 数据写入 Lance、Parquet 格式，并构建 FlatStor 模拟索引。

#### Scenario: 构建 Deep1M Lance 数据集
- **WHEN** 以 `--dataset deep1m --format lance` 运行
- **THEN** 在 `baselines/data/deep1m_lance/` 创建 LanceDB 表，含 `id`（int64）、`vector`（float32 array）、`payload`（binary，10KB 合成）三列

#### Scenario: 构建 COCO 100K Parquet 数据集
- **WHEN** 以 `--dataset coco_100k --format parquet` 运行
- **THEN** 在 `baselines/data/coco100k.parquet` 写入 `id`、`vector`、`payload`（JPEG binary）列，按 id 排序以优化行组读取

#### Scenario: 构建 FlatStor 模拟索引
- **WHEN** 以 `--dataset coco_100k --format flatstor` 运行
- **THEN** 在 `baselines/data/coco100k_flatstor.idx` 写入 numpy 数组（shape: [N, 2]，每行为 [byte_offset, payload_size]），复用 BoundFetch `.dat` 文件原始数据

### Requirement: Payload 检索基准测试脚本
系统 SHALL 在 `/home/zcq/VDB/baselines/payload_retrieval/run_payload_bench.py` 提供 payload 检索对比脚本，接受 FAISS/DiskANN 输出的 Top-K IDs，分别测量 FlatStor、Lance、Parquet 的检索延迟。

#### Scenario: FlatStor 模拟 payload 检索（B3）
- **WHEN** 以 `--ids results/faiss_topk_ids_coco100k.npy --backend flatstor --topk 10` 运行
- **THEN** 对每个 query 的 Top-K IDs 做 offset 查找（O(1) numpy index）+ `os.pread`，输出 `avg_latency_ms`、`p50_latency_ms`、`p99_latency_ms` 到 `results/payload_retrieval.csv`

#### Scenario: Lance payload 检索（B4）
- **WHEN** 以 `--ids results/faiss_topk_ids_deep1m.npy --backend lance --topk 10` 运行
- **THEN** 对每个 query 调用 `table.take(ids)` 检索 payload，记录端到端延迟并追加到 CSV

#### Scenario: Parquet payload 检索（B5）
- **WHEN** 以 `--ids results/faiss_topk_ids_deep1m.npy --backend parquet --topk 10` 运行
- **THEN** 对每个 query 用 `pyarrow.parquet.read_table(filters=[("id","in",ids)])` 读取行，记录延迟并追加到 CSV

#### Scenario: 多 backend 批量运行
- **WHEN** 以 `--backend flatstor,lance,parquet` 参数运行
- **THEN** 依次运行三个 backend，将所有结果追加到同一 CSV 文件

### Requirement: E2E 对比汇总脚本
系统 SHALL 在 `/home/zcq/VDB/baselines/e2e/run_e2e_comparison.py` 提供端到端对比脚本，合并 Layer 1 和 Layer 2 结果生成 E2E 对比表。

#### Scenario: 生成 E2E 对比 CSV
- **WHEN** 读取 `results/vector_search.csv` 和 `results/payload_retrieval.csv`
- **THEN** 计算各组合（FAISS+FlatStor、FAISS+Lance、FAISS+Parquet、DiskANN+FlatStor 等）的 E2E latency，连同 BoundFetch E2E 数据，写入 `results/e2e_comparison.csv`

#### Scenario: 包含 BoundFetch E2E 参照行
- **WHEN** 提供 `--boundfetch-result <bench_e2e_output.json>` 参数
- **THEN** 解析 bench_e2e JSON，将 BoundFetch 的 avg_query_ms 和 recall@10 作为参照行加入 E2E 对比表

### Requirement: 结果 CSV 统一格式
所有 baseline 脚本 SHALL 将结果写入 `/home/zcq/VDB/baselines/results/` 下的 CSV，格式统一：`system`、`dataset`、`topk`、`param`（nprobe 或其他超参）、`recall@1`、`recall@5`、`recall@10`、`avg_latency_ms`、`p50_latency_ms`、`p99_latency_ms`、`notes`。

#### Scenario: 文件不存在时自动创建带表头的 CSV
- **WHEN** 输出 CSV 路径不存在
- **THEN** 脚本自动创建文件并写入表头

#### Scenario: 多次运行结果可追加
- **WHEN** 以不同参数多次运行同一脚本
- **THEN** 每次运行追加新行，不覆盖已有结果
