# 提案：纯向量搜索 Benchmark

## 问题

当前仅有 `bench_e2e` 一个端到端 benchmark，它依赖完整的 payload 数据（JSONL + io_uring）。这意味着：

1. **无法测试纯向量数据集**：DEEP1M、SIFT1M、GIST1M 等标准 ANN-Bench 数据集使用 `.fvecs`/`.ivecs` 格式，没有 payload，无法用 `bench_e2e` 测试。
2. **测试流程不分层**：应当先在向量侧验证 recall，确认无误后再跑端到端。目前跳过了向量侧验证这一步。
3. **缺少 `.fvecs`/`.ivecs` 读取能力**：IO 模块只有 `.npy` 和 JSONL reader。
4. **CRC early-stop 未在聚类探测中接入**：bench_e2e 中 CRC 是通过 OverlapScheduler 接入的，纯向量 benchmark 需要在 probe 循环中直接使用 `CrcStopper`。

## 方案

### A. 新增 fvecs/ivecs IO 库

在 `vdb::io` 中新增 `vecs_reader.h`/`.cpp`，提供：
- `LoadFvecs(path)` → float 矩阵（复用 `NpyArrayFloat` 类型）
- `LoadIvecs(path)` → int32 矩阵（新增 `VecsArrayInt32` 类型）

### B. 新增 `bench_vector_search`

纯内存向量搜索 benchmark，流程：KMeans → RaBitQ Encode → 校准 → Probe+Rerank → Recall vs GT。

- **数据加载**：CLI 参数指定 `--base`、`--query`、`--gt` 路径，按扩展名（`.fvecs`/`.npy`）自动选择 reader
- **GT 处理**：`.ivecs` 文件存在则加载，否则暴力 L2 自算
- **CRC**：内联计算（`CalibrateWithRaBitQ`），在 probe 循环中通过 `CrcStopper::ShouldStop()` 实现 early-stop

### C. 测试分层

```
任何新数据集 (fvecs/npy)     有 payload 的数据集 (COCO)
        │                              │
        ▼                              ▼
  bench_vector_search            bench_e2e
  向量侧 recall 验证    ──OK──▶  端到端验证
```

### D. 现有 benchmark 适配 fvecs

`bench_rabitq_accuracy` 和 `bench_rabitq_diagnostic` 的数据加载硬编码了 `--dataset` 目录下的 `image_embeddings.npy` / `query_embeddings.npy`。改为支持 `--base`/`--query` CLI 参数 + 扩展名探测，同时保留 `--dataset` 向后兼容。

## 范围

- 新增 `vecs_reader.h`/`.cpp`（fvecs + ivecs 读取）+ `LoadVectors` 扩展名探测函数
- 新增 `bench_vector_search.cpp`（纯向量搜索 benchmark，含 CRC early-stop）
- 改造 `bench_rabitq_accuracy.cpp`：支持 `--base`/`--query` + fvecs 格式
- 改造 `bench_rabitq_diagnostic.cpp`：支持 `--base`/`--query` + fvecs 格式
- CMakeLists.txt 更新
- 单元测试：vecs_reader 往返测试

## 不在范围内

- 修改 `bench_e2e` 的逻辑
- 修改 OverlapScheduler 或在线查询路径
- 新增数据集下载工具
