# Baseline 实验计划（磁盘模式重构版）

**日期**：2026-04-11（v5: 全面重构为 disk-based baseline，新增 Deep8M，切换 labnew Python 3.12 环境）
**定位**：本文件是 BASELINE_PLAN 的最新权威版本。v4 的纯内存 FAISS/DiskANN 测试（见下方附录 A）仅保留作为"内存上界"的参考数据，不进入论文主表。
**问题重述**：BoundFetch 是 disk-based vector search + payload 系统，其主要论点是 I/O–compute 重叠和 SafeOut 剪枝能在冷/温 I/O 场景下显著降低 E2E 延迟。当前 v4 baseline 全部为 pure-memory FAISS IVF-PQ/IVF-Flat，与 BoundFetch 的 disk 行为不对称，不能支撑论文的核心对比。本版本重构所有向量搜索 baseline 为磁盘模式，并新增 Deep8M 以在 dataset > page cache 时形成真实的 I/O 压力。

---

## 0. v5 与 v4 的差异摘要

```
                                v4 (已跑)              v5 (本版本)
═══════════════════════════════════════════════════════════════════
Python 环境                     lab (py3.8)           labnew (py3.12)
──────────────────────────────────────────────────────────────────
diskannpy                       0.4.0 (老API, 破)     >= 0.7  (现代 API)
──────────────────────────────────────────────────────────────────
向量搜索 baseline               pure memory           disk-mode
  FAISS                         IndexIVFPQ in RAM     OnDiskInvertedLists
  DiskANN                       build 失败            真正 disk mode
──────────────────────────────────────────────────────────────────
数据集                          deep1m + coco100k     + deep8m
──────────────────────────────────────────────────────────────────
Deep1M 聚类数                   4096                  4096 (不变)
Deep8M 聚类数                   N/A                   12800 (新)
COCO 100K 聚类数                2048                  2048 (不变)
──────────────────────────────────────────────────────────────────
I/O 协议                        未区分                区分 COLD vs WARM
──────────────────────────────────────────────────────────────────
E2E 数据集                      deep1m + coco100k     coco100k 为主
```

---

## 1. Paper Claim 与 Baseline 的对应关系

| Claim | 需要什么 baseline 来证伪/支撑 | 所属 block |
|-------|------------------------------|-----------|
| **C1** SafeOut 剪枝减少 40-60% I/O 体积 | 对比"FAISS disk-mode + 被动 payload 读"（无 SafeOut）| B1 + B5 |
| **C2** io_uring 重叠调度 >80% overlap | BoundFetch 自身的 pipeline_stats.overlap_ratio，无需外部 baseline | — |
| **C3** 2-3× 低于朴素分离布局 | 自身消融（separate .clu/.dat vs interleaved），不在本 plan | 另开 |
| **C5** CRC 早停 30-70% 跳过率 | 自身统计 `early_stopped_pct`，无需外部 baseline | — |
| **C6** E2E 优于现有系统 | FAISS(disk) + FlatStor/Lance 与 DiskANN(disk) + FlatStor 作为强 baseline，全部在 **COLD cache** 下测 | B1 + B2 + B5 |

**本 baseline plan 只负责 C1 / C6**。C3 是内部消融，C2/C5 靠自身指标。

---

## 2. 核心测试协议

### 2.1 冷启动 vs 温启动

```
┌────────────────────────────────────────────────────────────────┐
│  COLD cache protocol（主协议，进主表）                          │
│    对每次 measurement run：                                     │
│    1. sudo sync                                                │
│    2. echo 3 | sudo tee /proc/sys/vm/drop_caches               │
│    3. 重启 Python 进程                                          │
│    4. 跑 1000 个 query，取 avg/p50/p99 → 第一次运行包含首次     │
│       加载开销                                                  │
│    5. 每个 query 之间 **不** drop cache                        │
│    这个协议对应 "用户第一次查询时的冷启动延迟"                   │
│                                                                │
│  WARM cache protocol（辅助协议，进附录）                        │
│    1. 提前跑 100 个 warmup query 让 page cache 热起来           │
│    2. 清 latency 数组，重新跑 1000 个 query                    │
│    3. 取 steady-state latency                                   │
│    这个协议对应 "长时间运行的稳态性能"                           │
└────────────────────────────────────────────────────────────────┘
```

**为什么必须有 COLD**：Deep1M 和 COCO 100K 都小（< 1GB），完全在 page cache 内，WARM 测不出 disk 性能——只有 COLD 有意义。Deep8M 2.9GB 仍可能被 cache，也需要 drop cache。

### 2.2 测试环境隔离

```
sudo 需求：drop_caches 必须 sudo。如果没有 sudo：
  Fallback: 每个 dataset 用 posix_fadvise(POSIX_FADV_DONTNEED) 清该文件的 page cache
  Python: os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
  C++:   posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)
  这个足以清掉目标索引文件的 page cache，足够用。
```

### 2.3 Query 规模

- 所有 baseline 用同一批 **1000 个 query**（从数据集 query 集均匀采样）
- COCO 100K 的 ground truth 改为离线预计算好存 `/home/zcq/VDB/data/coco_100k/groundtruth_top10.npy`，不再每次 brute-force（节省 ~125s）

---

## 3. 数据集矩阵

```
┌────────────┬──────────┬────────┬──────────┬─────────┬────────────┐
│  Dataset   │ Vectors  │  Dim   │ Size     │ nlist   │ Purpose    │
├────────────┼──────────┼────────┼──────────┼─────────┼────────────┤
│ Deep1M     │  1 M     │  96    │  384 MB  │ 4096    │ Smallref   │
│ Deep8M     │  8 M     │  96    │ 2.93 GB  │ **12800**│ 主力 disk │
│ COCO 100K  │  100 K   │  512   │  200 MB  │ 2048    │ E2E 专用   │
└────────────┴──────────┴────────┴──────────┴─────────┴────────────┘
```

**分工说明**：
- **Deep8M** 是本版本新引入的核心数据集，用于所有"disk-based 向量搜索 B1/B2"对比。2.93GB > RAM 对 I/O 不形成压力，但 > page cache 后部分页会被换出，COLD 协议下会显示真实磁盘 I/O 延迟。
- **Deep1M** 保留作为回归对照，验证小数据集下 baseline 是否稳定。
- **COCO 100K** 是 cross-modal 向量+caption payload，是 E2E 测试的主力数据集（Layer 1 + Layer 2 + BoundFetch）。

**Deep8M nlist=12800 的理由**：
- 经验法则 `nlist ≈ 4*sqrt(N)`：8M 下约 11313，取 12800（1.6 × 8000）
- 每个 cluster 平均 ~625 个 vector，与 Deep1M/nlist=4096 的 ~244 同数量级但更合理
- 用 10-20 bits for the inverted list index，比 4096 更精细，recall 曲线更漂亮

---

## 4. 两层对比架构（保持不变）

```
┌──────────────────────────────────────────────────────────────────┐
│  Layer 1：向量搜索（disk-mode only）                             │
│                                                                  │
│  BoundFetch (disk)  vs  FAISS IVF-PQ (disk)  vs  DiskANN (disk) │
│  → recall@10, latency, 输出 Top-K IDs                           │
└──────────────────────────────────────────────────────────────────┘
                           ↓ Top-K IDs
┌──────────────────────────────────────────────────────────────────┐
│  Layer 2：Payload 检索（沿用 v4）                                │
│                                                                  │
│  FlatStor-sim  vs  Lance  vs  Parquet                            │
│  → 给定 IDs，从磁盘取 payload 的延迟                             │
└──────────────────────────────────────────────────────────────────┘
                           ↓
┌──────────────────────────────────────────────────────────────────┐
│  Layer 3：E2E 组合（只测 COCO 100K）                              │
│                                                                  │
│  BoundFetch E2E  vs  (FAISS disk + FlatStor/Lance)               │
│                 vs  (DiskANN disk + FlatStor/Lance)              │
└──────────────────────────────────────────────────────────────────┘
```

---

## 5. 实验 Block 定义

### Block B1：FAISS 磁盘模式向量搜索（P0）

**Claim**：支撑 C6。证明 BoundFetch 在 COLD cache 下优于正统的 disk-based IVF。

**系统**：`faiss.IndexIVFPQ` + `faiss.OnDiskInvertedLists`

**为什么用 OnDiskInvertedLists 而不是 mmap**：
- mmap 的 page cache 行为不可控，cold 后一次预热就全热了，测不出每次 probe 真实的 disk I/O
- OnDiskInvertedLists 是 FAISS 官方的"显式磁盘"模式，倒排列表逐个 pread 从文件读入；中心向量和 PQ codebook 保留在 RAM
- 架构上和 BoundFetch 最可比：都是"probe cluster → pread disk"

**构建流程**（伪代码）：
```python
index = faiss.IndexIVFPQ(quantizer, D, nlist, m, 8)
index.train(base)
index.add(base)
# 转 on-disk
invlists = faiss.OnDiskInvertedLists(nlist, index.code_size, path_bin)
index.replace_invlists(invlists)
faiss.write_index(index, path_index)
```

**运行矩阵**：

| dataset  | nlist | PQ (m×bits) | nprobe          | query | protocol  |
|----------|-------|-------------|-----------------|-------|-----------|
| Deep1M   | 4096  | 24 × 8      | 32,64,128,256   | 1000  | COLD+WARM |
| Deep8M   | 12800 | 24 × 8      | 32,64,128,256,512| 1000  | COLD+WARM |
| COCO 100K| 2048  | 64 × 8      | 32,64,128,300   | 1000  | COLD+WARM |

**输出**：`results/vector_search.csv` 每行一个 (dataset, nprobe, protocol)，含 recall@1/5/10、latency、io_time（如果能分开测）。

**同时导出** Top-K faiss_fidx 到 `results/faiss_topk_fidx_{dataset}_nprobe{N}.npy` 供 B5 使用。

**成功判据**：
- Deep8M nprobe=128 时 recall@10 ≥ 0.90
- COLD vs WARM 延迟差异 ≥ 2×（否则说明测试协议没生效）

---

### Block B2：DiskANN 磁盘模式向量搜索（P0，升级为必跑）

**Claim**：支撑 C6。DiskANN 是 disk-based ANN 的标准 baseline，paper 必须对比。

**系统**：diskannpy >= 0.7（在 labnew env）
```python
import diskannpy as dap
dap.build_disk_index(
    data=...,
    distance_metric="l2",
    index_directory=...,
    complexity=100,
    graph_degree=64,
    search_memory_maximum=1.0,     # 1 GB RAM budget → 强制 disk 模式
    build_memory_maximum=32.0,
    num_threads=8,
    pq_disk_bytes=<D>,             # 96 for deep, 128 for coco
)
idx = dap.StaticDiskIndex(...)
idx.search(q, k=10, complexity=L_search, beam_width=4)
```

**运行矩阵**：

| dataset  | R  | L_build | L_search       | beam | query | protocol  |
|----------|----|---------|----------------|------|-------|-----------|
| Deep1M   | 64 | 100     | 50,100,200     | 4    | 1000  | COLD+WARM |
| Deep8M   | 64 | 100     | 50,100,200,300 | 4    | 1000  | COLD+WARM |
| COCO 100K| 64 | 100     | 50,100,200     | 4    | 1000  | COLD+WARM |

**输出**：追加到 `results/vector_search.csv`，系统名 `DiskANN`；同时保存 `results/diskann_topk_fidx_{dataset}_L{N}.npy` 供 B5 使用。

**风险**：
- diskannpy 0.7 是否真的能用 Python 3.12：先 `pip install diskannpy` 然后 `import diskannpy` 验证
- 如果 0.7 也构建失败，退回直接调用 `DiskANN` C++ binary（`build_disk_index` + `search_disk_index` 的 `apps/` 目录下有）

**成功判据**：
- 三个数据集都能 build 完 + load 成功 + 搜索
- Deep8M L_search=200 时 recall@10 ≥ 0.90
- COLD 下的 io_wait 占主延迟（否则说明 cache 没清干净）

---

### Block B5：Layer 2 Payload 检索（P0，沿用 v4 结果，只补 Deep8M）

**Claim**：支撑 C6。证明三类存储格式的 payload 检索延迟差异。

**已完成**（v4 跑完）：
- Deep1M: FlatStor 0.034ms, Lance 0.236ms, Parquet 跳过（10GB synthetic payload）
- COCO 100K: FlatStor 0.026ms, Lance 0.185ms, Parquet 7.524ms

**本版本新增**：
- Deep8M payload 数据构建：跳过大 synthetic payload，直接用 entity_id 作为 payload（8 bytes 每条）
  - 目的：disk-based 向量搜索的主流场景，payload 很可能只是一个 id → 实际 payload 大多从下游服务取
  - 这样 Deep8M payload I/O 量极小，主 bottleneck 是 vector search 本身
- Deep8M FlatStor index、Lance、Parquet 三个格式都构建
- 对 B1/B2 的 Top-K fidx 跑 payload bench

**运行矩阵**：

| dataset  | backend   | 输入 IDs 文件（来自）            | protocol  |
|----------|-----------|---------------------------------|-----------|
| Deep8M   | FlatStor  | faiss_topk_fidx_deep8m_nprobe256| COLD+WARM |
| Deep8M   | Lance     | 同上                            | COLD+WARM |
| Deep8M   | Parquet   | 同上                            | COLD      |
| Deep1M   | 复用 v4   | —                               | —         |
| COCO 100K| 复用 v4 + 补测 COLD | 重新跑 COLD 协议       | COLD      |

**输出**：`results/payload_retrieval.csv` 追加 Deep8M 三个 backend 结果，COCO 100K 追加 COLD 行。

---

### Block B-REF：纯内存 FAISS（v4 数据）仅作内存上界参考

**Claim**：支撑 C6 的 sanity check——证明 disk 模式和 memory 模式的差距是可理解的。

**动作**：不再重跑。v4 已有的结果（pure-memory FAISS IVF-PQ 和 IVF-Flat）在 COCO 100K / Deep1M 上的延迟保留在 `results/vector_search.csv` 并打标签 `notes="in-memory-reference-only"`。

**用途**：论文里只在 appendix 出现一张"disk vs memory upper bound 对照表"，解释 disk 模式下延迟上升的合理性。不进主表。

---

### Block B-E2E：Layer 3 E2E 组合（P0，只测 COCO 100K）

**Claim**：支撑 C6。论文主表的核心一行：BoundFetch E2E vs 所有组合。

**要比什么**：

| 系统组合                                   | 谁的搜索 | 谁的 payload | protocol |
|--------------------------------------------|----------|--------------|----------|
| BoundFetch (integrated)                    | BF       | BF           | COLD+WARM |
| FAISS-IVFPQ(disk) + FlatStor-sim           | FAISS(B1)| FlatStor(B5) | COLD+WARM |
| FAISS-IVFPQ(disk) + Lance                  | FAISS(B1)| Lance(B5)    | COLD+WARM |
| DiskANN(disk) + FlatStor-sim               | DiskANN  | FlatStor     | COLD+WARM |
| DiskANN(disk) + Lance                      | DiskANN  | Lance        | COLD+WARM |

**注意**：E2E 组合是线性加总 `E2E = vec_search + payload`，这相当于串行调用。BoundFetch 的优势在于 I/O-compute overlap，所以 BF 的 `avg_query_ms` 会比 `vec_search + payload` 简单加总要低。论文里这点是核心论据。

**输出**：`results/e2e_comparison.csv` 含上表所有组合 + COLD/WARM 两列。

---

## 6. 执行顺序（Run Order）

```
Milestone M0 — Environment validation（半天）
  - labnew env 验证：faiss 1.8+, diskannpy >=0.7, lancedb, pyarrow
  - 确认 diskannpy 能 import + build 一个 100K toy 数据集
  - 决策 gate：若 diskannpy 0.7 仍失败，退回 C++ binary

Milestone M1 — 数据准备（1 天）
  - Deep8M 数据验证：base.fvecs / query.fvecs / groundtruth.ivecs 可加载
  - 预计算 COCO 100K groundtruth → 存 npy
  - 构建 Deep8M 三种 payload 格式（FlatStor/Lance/Parquet, 只存 entity_id）
  - 构建 Deep8M BoundFetch 自己的 .clu/.dat 索引（在另一个 change 里，不在本 plan）

Milestone M2 — Layer 1 disk-mode 向量搜索（1.5 天）
  - B1: FAISS OnDiskInvertedLists
      Day 1: Deep1M (快) + COCO 100K
      Day 2 AM: Deep8M (慢，主压力)
  - B2: DiskANN disk mode
      Day 2 PM: 全部三个数据集
  - 决策 gate：COLD vs WARM 延迟比 ≥ 2× → 协议有效，可进下一步

Milestone M3 — Layer 2 payload + Layer 3 E2E（1 天）
  - B5: Deep8M payload 三后端 + COCO 100K COLD 重测
  - E2E: 生成最终 comparison 表

Milestone M4 — 分析与输出（半天）
  - 更新 `results/analysis.md`
  - 更新 `EXPERIMENT_TRACKER_CN.md` 第 0 阶段
  - 写 "COLD vs WARM 延迟比率" 附录章节
  - 决策 gate：如果 BoundFetch E2E 在 COLD 下仍显著 > FAISS+FlatStor，说明优化方向是纯 CPU；反之则论文论点强

总估：4 天
```

---

## 7. 预期主表结构

```
┌─────────────────────────────────────────────────────────────────────────┐
│ TABLE 1：向量搜索 COLD cache @ recall@10 ≥ 0.90（主表）                 │
├──────────────────────┬──────────┬──────────┬──────────┬─────────────────┤
│ 系统                  │  Deep1M  │  Deep8M  │ COCO100K │  备注            │
├──────────────────────┼──────────┼──────────┼──────────┼─────────────────┤
│ BoundFetch (disk)    │   ??     │   ??     │   ??     │ 本系统           │
│ FAISS IVFPQ (disk)   │   ??     │   ??     │   ??     │ FAISS on-disk   │
│ DiskANN (disk)       │   ??     │   ??     │   ??     │ 图索引 disk 模式 │
└──────────────────────┴──────────┴──────────┴──────────┴─────────────────┘

TABLE 2：E2E COLD cache @ recall@10 ≥ 0.90 (COCO 100K only)
  BoundFetch                 ??ms  (集成)
  FAISS(disk) + FlatStor    ??ms  (串行加总)
  FAISS(disk) + Lance       ??ms
  DiskANN(disk) + FlatStor  ??ms
  DiskANN(disk) + Lance     ??ms

TABLE 3 (appendix)：Memory upper bound 参考
  FAISS in-memory (v4 数据)  0.15-1.4ms    内存上界，论文不做主对比

TABLE 4 (appendix)：COLD vs WARM 延迟比
  系统 × dataset × nprobe，展示 I/O 占比
```

---

## 8. 风险与 Mitigation

| 风险 | 概率 | 影响 | 应对 |
|------|------|------|------|
| diskannpy 0.7 仍有 bug | 中 | 高 | 退回 C++ binary，调用 `search_disk_index` 写 subprocess wrapper |
| Deep8M 在本机磁盘装不下 或 跑得太慢 | 低 | 高 | 数据已有 2.9GB，检查剩余磁盘 > 10GB 即可；nprobe ≤ 512 保证查询可接受 |
| sudo drop_caches 不可用 | 中 | 中 | 用 posix_fadvise 的 fallback 足够 |
| Deep8M 用 nlist=12800 导致 training 太慢 | 中 | 中 | 用 mini-batch training (只训练 200K 子样本) |
| COLD 和 WARM 没有显著差别 | 低 | 高 | 说明 page cache 太大，用 cgroup 限制进程内存 ≤ 1GB |
| E2E cold 组合中 BoundFetch 反而更慢 | 低 | 关键 | 说明主要声称需要修正——这是论文价值的核心判断，必须面对 |

---

## 9. Baseline 结果 → 优化方向决策树（v5 更新）

```
B1/B2 COLD 结果：
├── BoundFetch(disk) COLD < FAISS(disk) COLD
│   → 核心声称成立（I/O-compute overlap 的胜利）
│   → 论文主表第一行就是这个对比，直接写
│
├── BoundFetch(disk) COLD ≈ FAISS(disk) COLD
│   → 纯搜索没有优势，优势在 E2E 集成
│   → 需要强调 E2E table：overlap 带来的 payload 隐藏
│
└── BoundFetch(disk) COLD > FAISS(disk) COLD
    → 需要优化：BoundFetch 的 .clu 布局、CRC 解码、
       ConANN 热路径 CPU 开销
    → profile 目标：probe_time, crc_decode_time

E2E COLD 结果：
├── BoundFetch E2E < Σ(FAISS + FlatStor/Lance) COLD
│   → SafeOut 剪枝 + overlap 都有效，论文赢
│
└── BoundFetch E2E ≈ Σ(FAISS + FlatStor)
    → overlap 没发挥作用，检查：
       1. avg_io_wait_ms 是否还是 <10%？
       2. COLD cache 下 vs WARM overlap 比率变化？
```

---

## 10. v5 实际结果（2026-04-13 更新）

### 10.1 已完成范围

- 已完成 `bench-disk-mode-baseline` 对应的 warm 协议主线验证：FAISS OnDisk、DiskANN disk-mode、Deep8M payload、COCO 100K E2E 汇总。
- 本轮不再新增 `cold` / `semi_cold` 结果；由于环境约束，本 change 先以 warm steady-state 为主，并把真实磁盘叙事保留给后续补测。
- v4 的 pure-memory 结果继续只作为 appendix 里的 memory upper bound，不参与主表。

### 10.2 当前权威结果锚点

**COCO 100K warm E2E：**

| System | Key config | recall@10 | E2E (ms) | 备注 |
|--------|------------|-----------|----------|------|
| BoundFetch | nlist=2048, nprobe=200, qd=64, shared | 0.8984 | 1.136-1.141 | submit-path 优化后结果 |
| FAISS-IVFPQ-disk + FlatStor-sim | nprobe=32 | 0.7470 | 1.3124 | 精度受 PQ 限制 |
| DiskANN-disk + FlatStor-sim | L_search=50 | 1.0000 | 3.7899 | 图索引 warm 下明显更慢 |

**BoundFetch nprobe sweep（COCO 100K, warm, qd=64, shared）：**

| nprobe | recall@10 | avg (ms) | probe (ms) | uring_submit (ms) | submit_calls |
|--------|-----------|----------|------------|-------------------|--------------|
| 50  | 0.8018 | 0.6391 | 0.2169 | 0.2618 | 13.376 |
| 100 | 0.8748 | 0.9323 | 0.3440 | 0.3890 | 19.240 |
| 200 | 0.8984 | 1.1414 | 0.4276 | 0.4834 | 23.038 |
| 300 | 0.8988 | 1.1530 | 0.4306 | 0.4869 | 23.222 |
| 500 | 0.8988 | 1.1595 | 0.4301 | 0.4861 | 23.222 |

### 10.3 本轮结论修正

- 2026-04-12 的旧结论是 `BoundFetch WARM E2E=5.42ms > DiskANN+FlatStor=3.79ms`，当时仍受 rotation 持久化缺陷和 submit-path 开销影响。
- 经过 rotation 修复和 submit batching 优化后，BoundFetch 在同类 warm workload 下已降到约 1.14ms，优于当前的 DiskANN+FlatStor，且接近 FAISS-IVFPQ-disk+FlatStor。
- 因此，本 change 的结论应更新为：`warm steady-state 下 BoundFetch 已具备竞争力，后续优化重点是 submit-side CPU，而不是设备 I/O wait`。

### 10.4 后续动作

- 补 `cold` / `semi_cold`：用于验证论文中的真实 disk I/O 叙事，而不是继续放大 warm micro-optimization。
- 若继续沿 warm 路径优化，优先级应放在：
  1. 进一步减少 `io_uring_enter` 次数和 submit calls。
  2. 合并/延后不必要的 payload fetch submit。
  3. 继续压缩 `ProbeCluster()` 的固定成本，使 nprobe=200 档位进入稳定 1.0ms 左右区间。

---

## 附录 A：v4 历史结果（保留作参考，不进论文主表）

v4 在 lab (py3.8) 下跑的 pure-memory 结果：

| System            | Dataset   | nprobe | recall@10 | avg (ms) |
|-------------------|-----------|--------|-----------|----------|
| FAISS-IVFPQ       | Deep1M    | 256    | 1.000     | 1.334    |
| FAISS-IVFPQ       | COCO 100K | 150    | 0.748     | 0.831    |
| FAISS-IVFFLAT     | COCO 100K | 32     | 0.989     | 0.155    |
| FAISS-IVFFLAT     | COCO 100K | 128    | 1.000     | 0.388    |
| FAISS-IVFFLAT     | COCO 100K | 300    | 1.000     | 0.859    |
| FlatStor payload  | Deep1M    | —      | —         | 0.034    |
| FlatStor payload  | COCO 100K | —      | —         | 0.026    |
| Lance payload     | Deep1M    | —      | —         | 0.236    |
| Lance payload     | COCO 100K | —      | —         | 0.185    |
| Parquet payload   | COCO 100K | —      | —         | 7.524    |

**结论**：v4 数据在 disk-based paper 里只作为"in-memory upper bound"参考，不做主对比。DiskANN 在 v4 下完全失败。

---

## 附录 B：数据集路径速查

```
/home/zcq/VDB/data/
├── deep1m/
│   ├── deep1m_base.fvecs          (371 MB)
│   ├── deep1m_query.fvecs
│   └── deep1m_groundtruth.ivecs
├── deep8m/
│   ├── deep8m_base.fvecs          (2.9 GB)
│   ├── deep8m_base.bin
│   ├── deep8m_query.fvecs
│   ├── deep8m_query_10k.fvecs
│   ├── deep8m_groundtruth.ivecs
│   └── deep8m_centroids_16.fvecs  (已有 IVF16 中心，不用)
└── coco_100k/
    ├── image_embeddings.npy       (512d × 100K)
    ├── image_ids.npy
    ├── query_embeddings.npy
    └── metadata.jsonl             (caption payload)

/home/zcq/VDB/baselines/
├── data/                      ← 构建的 Lance/Parquet/FlatStor 输出
├── results/                   ← CSV/npy/index 缓存
├── vector_search/             ← run_faiss_*.py, run_diskann.py
├── payload_retrieval/         ← run_payload_bench.py, build_datasets.py
└── e2e/                       ← run_e2e_comparison.py
```

---

## 附录 C：Python 环境切换

```bash
# 主环境（已跑 v4）
conda activate lab          # Python 3.8.20

# v5 新环境
conda activate labnew       # Python 3.12

# 验证 diskannpy
python -c "import diskannpy; print(diskannpy.__version__)"

# 所有 v5 baseline 脚本运行时都用 labnew env：
/home/zcq/anaconda3/envs/labnew/bin/python baselines/vector_search/run_faiss_ivfpq_disk.py ...
```
