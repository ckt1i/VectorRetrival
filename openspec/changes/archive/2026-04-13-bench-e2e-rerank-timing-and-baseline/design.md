## Context

**当前计时状态**

`bench_e2e.cpp` 目前输出的 `cpu=3.975ms` = `total_time - io_wait_time`，包含：
- `probe_time_ms`（2.823ms）：Stage 1/2 RaBitQ 分类，已单独统计
- rerank CPU（≈1.15ms）：`ConsumeVec`/`ConsumeAll` 对原始向量做精确 L2Sqr 的计算时间，**未单独统计**
- 其他开销（AssembleResults、FinalDrain sync 路径等）

`io_wait_time_ms` 仅计时 `WaitAndPoll()` 调用本身（io_uring 阻塞等待），`ConsumeVec`/`ConsumeAll` 在 `DispatchCompletion` 中发生在 timer 截止之后，所以 io_wait 已是纯等待时间，无需拆分。

**Baseline 目录设计**

所有 baseline 代码和结果统一放在 `/home/zcq/VDB/baselines/`，不在 VectorRetrival 主目录下。FlatStor 若需重新编译不会污染主项目。

**对比测试的两层结构**

BoundFetch 的核心贡献是**搜索与 payload I/O 的集成调度**，因此 baseline 需要分两层独立建立：

```
Layer 1: 向量搜索
  BoundFetch search  vs  FAISS IVF-PQ  vs  DiskANN
  → 产出：recall, search latency, Top-K entity IDs

Layer 2: Payload 检索（接受 Layer 1 的 Top-K IDs）
  FlatStor (VLDB 2025)  vs  Lance  vs  Parquet
  → 产出：给定 IDs，从各存储格式取 payload 的延迟

Layer 1+2 合计 = E2E latency
BoundFetch E2E  vs  (FAISS + FlatStor)  vs  (FAISS + Lance)  etc.
```

这样 FlatStor/Lance/Parquet baseline **直接对接 FAISS IVF-PQ 的搜索结果 IDs**，测量在相同候选集下不同存储方式的 payload 检索延迟差异。

## Goals / Non-Goals

**Goals:**
- 新增 `rerank_cpu_ms` 字段，单独测量 `ConsumeVec`/`ConsumeAll` 的纯 CPU 时间
- 在 `/home/zcq/VDB/baselines/` 下建立完整的两层 baseline 套件：
  - **Layer 1**：B1 FAISS IVF-PQ + B2 DiskANN（向量搜索）
  - **Layer 2**：B3 FlatStor + B4 Lance + B5 Parquet（payload 检索，接 Layer 1 Top-K IDs）
- 将各层结果汇总到 `/home/zcq/VDB/baselines/results/`，支持 E2E 对比表

**Non-Goals:**
- 不拆分 `io_wait_time_ms`（已是纯等待时间）
- 不实现 pread flat 单独脚本（已被 FlatStor/Lance/Parquet 覆盖）
- 不在 VectorRetrival 目录下创建 baseline 相关文件
- 不实现 BoundFetch 行存搜索（已决策跳过）
- 不覆盖 SIFT10M、Deep10M 等大规模数据集（留后续实验阶段）

## Decisions

### D1：rerank_cpu_ms 计时位置

**选择**：在 `OverlapScheduler::DispatchCompletion()` 内，对 `VEC_ONLY` 和 `VEC_ALL` 分支的 `reranker.ConsumeVec()` / `reranker.ConsumeAll()` 调用加 `std::chrono::steady_clock` 计时，累加到 `ctx.stats().rerank_cpu_ms`。

**理由**：`DispatchCompletion` 已拥有 `ctx` 引用，无需改动 `RerankConsumer` 接口；与 `probe_time_ms` 的计时粒度对称，overhead ≈ 2×20ns/call，可忽略。

### D2：Baseline 目录结构

```
/home/zcq/VDB/baselines/
├── vector_search/
│   ├── run_faiss_ivfpq.py        # B1: FAISS IVF-PQ，输出 recall/latency + Top-K IDs
│   └── run_diskann.py            # B2: DiskANN（Optional）
├── payload_retrieval/
│   ├── build_datasets.py         # 将 Deep1M / COCO 100K 写入 Lance / Parquet 格式
│   ├── build_flatstor_index.py   # 构建 ID→(offset,size) 索引（模拟 FlatStor VTable）
│   └── run_payload_bench.py      # 接受 Top-K IDs，测 FlatStor / Lance / Parquet 检索延迟
├── e2e/
│   └── run_e2e_comparison.py     # 合并 Layer1 + Layer2，生成 E2E 对比表
├── requirements.txt
└── results/
    ├── vector_search.csv
    ├── payload_retrieval.csv
    ├── e2e_comparison.csv
    └── analysis.md
```

### D3：FAISS IVF-PQ Top-K IDs 传递方式

FAISS 搜索结果的 index（0-based vector ID）即为 entity ID。搜索完成后：
1. 将每个 query 的 Top-K IDs 写入 `results/faiss_topk_ids.npy`（shape: [Q, K]）
2. `run_payload_bench.py` 从该文件读取 IDs，用于 FlatStor / Lance / Parquet 检索

这样两层脚本解耦，Layer 2 可独立重跑（无需重新搜索）。

### D4：三种 Payload 格式的实现选择

**FlatStor（B3）**：
- 构建 ID→(offset_in_dat, payload_size) 的 numpy 数组（模拟 FlatBuffers VTable 的 O(1) 随机定位）
- 对 Top-K IDs 做 `os.pread(dat_fd, size, offset)` 批量读取
- 标注：这是 FlatStor 核心 I/O 路径的近似，不含其列存压缩；体现 O(1) offset 查找 + pread 的延迟

**Lance（B4）**：
- 使用 `lancedb` 将向量 + payload 写入 Lance 格式（`db.create_table`）
- 用 `table.take(ids)` 做随机行访问，LanceDB 内部优化了 Top-K 随机读
- 测量 `take()` 端到端延迟（包含 Lance 内部 I/O 调度）

**Parquet（B5）**：
- 使用 `pyarrow.parquet` 写入，按 entity ID 排序分组
- 读取时用 row group filter，预期延迟较高（行组粒度访问），作传统格式对照
- 也可用 `pandas.read_parquet` + `filters=[("id", "in", top_k_ids)]` 对比

### D5：数据集准备

- **Deep1M**：384-dim float32 向量（base.fvecs），合成 10KB payload（随机字节，模拟 embedding + metadata），写入 Lance/Parquet
- **COCO 100K**：512-dim 向量 + 实际 JPEG payload（来自 `.dat` 文件），写入 Lance/Parquet
- FlatStor index 直接复用 BoundFetch 已有的 `.dat` 文件 + 构建 ID→offset 映射

### D6：E2E 对比计算方式

```
BoundFetch E2E:          bench_e2e 输出的 avg_query_ms（已含搜索+payload fetch）
FAISS + FlatStor E2E:    faiss_search_ms + flatstor_topk_fetch_ms
FAISS + Lance E2E:       faiss_search_ms + lance_take_ms
FAISS + Parquet E2E:     faiss_search_ms + parquet_filter_ms
DiskANN + FlatStor E2E:  diskann_search_ms + flatstor_topk_fetch_ms
```

`run_e2e_comparison.py` 读取各结果 CSV，计算组合 E2E，输出 Pareto 表格。

## Risks / Trade-offs

- **COCO 100K L3 cache 污染**：505MB 数据集可能驻留 L3 cache，HOT=COLD 现象真实存在；在 Deep1M + 合成 payload（>1GB）上运行以触发真实磁盘 I/O
- **FlatStor sim 近似误差**：不含列存压缩和 mmap 预读优化，实测延迟偏保守；在 analysis.md 中标注
- **Lance 版本差异**：LanceDB API 在不同版本间有变化，固定 `lancedb>=0.3` 版本
- **DiskANN 安装依赖重**：标记为 Optional，若安装失败用 hnswlib 内存搜索替代或跳过
- **Parquet 随机读天然慢**：预期 10~100× 慢于 Lance/FlatStor，仍需运行以建立"传统格式下界"对比点

## Migration Plan

1. 修改 `include/vdb/query/search_context.h`：添加 `rerank_cpu_ms`
2. 修改 `src/query/overlap_scheduler.cpp`：DispatchCompletion 计时
3. 修改 `benchmarks/bench_e2e.cpp`：输出新增 `rerank_cpu_ms`
4. 重编译 `build-bench-nomkl`，COCO 100K 验证
5. 在 `/home/zcq/VDB/baselines/` 下创建脚本（不修改 VectorRetrival）
6. 构建数据格式（Lance/Parquet/FlatStor index）
7. 运行 Layer 1 → 保存 Top-K IDs → 运行 Layer 2 → 生成 E2E 对比

## Open Questions

- Deep1M 是否有 entity ID 与向量 index 的对应关系？→ FAISS 返回的 index 即 0-based ID，直接用于 Lance/Parquet 查找，无需额外映射
- COCO 100K JPEG payload 如何写入 Lance/Parquet？→ 存为 binary 列（`pa.binary()`），Lance 和 Parquet 均支持
