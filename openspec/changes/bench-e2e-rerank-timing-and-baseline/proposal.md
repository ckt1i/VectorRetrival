## Why

当前 `bench_e2e` 的计时代码将 rerank 阶段的 CPU 计算与 I/O 等待合并统计为 `cpu_time`，无法区分瓶颈来源；同时缺乏与 FAISS IVF-PQ、DiskANN（向量搜索侧）以及 FlatStor、Lance、Parquet（payload 检索侧）等 SOTA baseline 的量化对比，导致 BoundFetch 的优化方向不明确。

## What Changes

- **Step 1 — bench_e2e 计时拆分**：在 rerank 阶段新增独立计时器，分别记录：
  - `rerank_cpu_ms`：等待 io_uring 完成后，对原始向量做精确距离计算的纯 CPU 时间（`ConsumeVec`/`ConsumeAll`）
  - 将原 `cpu_time` 输出扩展为 `probe_cpu_ms` + `rerank_cpu_ms` + 其他开销
  - 汇总输出新增字段，HOT/COLD 对比表同步更新

- **Step 2 — Baseline 对照测试**（所有代码和结果在 `/home/zcq/VDB/baselines/`）：
  - **向量搜索 baseline**：
    - **B1 FAISS IVF-PQ**：Python/mmap，Deep1M + COCO 100K，输出 recall/latency + Top-K IDs
    - **B2 DiskANN**：Python/磁盘索引，同上（Optional）
  - **Payload 检索 baseline**（接收 B1/B2 的 Top-K IDs，测 payload 取回延迟）：
    - **B3 FlatStor**：FlatBuffers VTable offset 索引 + pread，模拟 VLDB 2025 SOTA
    - **B4 Lance**：`lancedb` / `pylance` 的 `take(ids)` 随机访问
    - **B5 Parquet**：`pyarrow.parquet` 行级读取（预期较慢，作传统格式对照）
  - **E2E 对比**：BoundFetch 集成方案 vs（B1/B2 搜索 + B3/B4/B5 payload 检索）组合

## Capabilities

### New Capabilities
- `rerank-timing`: bench_e2e 新增 rerank 阶段分离计时（rerank_cpu_ms）
- `baseline-runner`: 向量搜索 + payload 检索 baseline 脚本套件（在 `/home/zcq/VDB/baselines/`）

### Modified Capabilities
- （无 spec 级别行为变更）

## Impact

- **代码变更**：`include/vdb/query/search_context.h`（新增字段）、`src/query/overlap_scheduler.cpp`（计时拆分）、`benchmarks/bench_e2e.cpp`（输出更新）
- **新目录**：`/home/zcq/VDB/baselines/`（Python 脚本、数据格式构建、结果 CSV，不影响 VectorRetrival 主目录）
- **依赖**：需安装 `faiss-cpu`、`lancedb`、`pyarrow`、`pylance`；`diskannpy` 为可选依赖
- **数据**：Deep1M（已有）、COCO 100K（已有）；各 baseline 需将数据写入对应格式（Lance dataset、Parquet 文件、FlatStor offset 索引）
- **输出**：`/home/zcq/VDB/baselines/results/`（向量搜索 CSV + payload 检索 CSV + E2E CSV）
