# 实验追踪表

**日期**：2026-04-10（v4: 新增 baseline 对比阶段）
**系统**：BoundFetch

---

## 状态图例
- [ ] 未开始
- [~] 进行中
- [x] 完成
- [!] 阻塞
- [-] 跳过（含原因）

## 已有数据锚点
- [x] COCO 100K, top_k=10, bits=4: recall@10=0.8925, SafeOut S1=90.54%, latency=1.738ms
- [x] Deep1M, top_k=10, bits=4: recall@10=0.9230, SafeOut S1=93.79%, 早停 73%, latency=0.814ms
- [x] Deep1M, top_k=20, bits=4: recall@10=0.9630, SafeOut S1=97.91%, 早停 21%, latency=1.547ms

## 已有数据集
- [x] Deep1M: 1M vectors, 96-dim, 有 groundtruth
- [x] COCO 100K: 100K vectors, 512-dim, 真实 JPEG payload (~16GB)
- [x] SIFT1M, GIST1M, SIFT10M, Deep10M 等纯向量数据集

---

## 第 0 阶段：Baseline 对比（新增，目标：2-3 天）

### 环境准备 ✅
- [x] 创建 Python baseline 环境（conda lab，Python 3.8.20）
- [x] 安装 faiss-cpu 1.8.0, diskannpy 0.4.0, lancedb 0.6.13, pyarrow 15.0.0, numpy, pandas

### B1：FAISS IVF-PQ / IVF-Flat baseline ✅（完成 2026-04-11）
- [x] Deep1M: IVF-PQ(nlist=4096)，nprobe={32,64,128,256}，recall@10=1.000，avg=0.21–1.33ms
- [x] COCO 100K: IVF-PQ recall@10 上限=0.75（PQ 量化误差），改用 IVF-Flat
- [x] COCO 100K: IVF-Flat(nlist=2048)，nprobe={32,64,128,200,300,500}，recall@10=0.989–1.000，avg=0.15–1.39ms
- [x] 保存 Top-K fidx 文件（faiss_topk_fidx_{dataset}_nprobe{N}.npy）

### B2：FlatStor payload baseline ✅（完成 2026-04-11）
- [x] Deep1M: 10KB 合成 payload，flat binary + offset 索引，pread Top-10 avg=0.034ms
- [x] COCO 100K: BoundFetch .dat caption payload，pread Top-10 avg=0.026ms
- [x] → 建立 payload 检索下界

### B3：DiskANN baseline ⚠️（跳过）
- [x] Deep1M 索引构建成功（1M pts / 118s）
- [x] diskannpy 0.4.0 API 不兼容：数据适合内存时生成 _mem.index，load_index() 只支持 _disk.index
- [x] 记录 diskann_unavailable 至 results/vector_search.csv

### B4：Lance payload baseline ✅（完成 2026-04-11）
- [x] Deep1M: lance.dataset.take() Top-10 avg=0.236ms
- [x] COCO 100K: lance.dataset.take() Top-10 avg=0.185ms

### B5：端到端组合 ✅（完成 2026-04-11）
- [x] FAISS-IVF-Flat + FlatStor on COCO: recall=0.989, E2E=0.181ms
- [x] FAISS-IVF-Flat + Lance on COCO: recall=0.989, E2E=0.340ms
- [x] FAISS-IVF-PQ + FlatStor on Deep1M: recall=1.000, E2E=0.243ms
- [x] FAISS-IVF-PQ + Lance on Deep1M: recall=1.000, E2E=0.460ms
- [x] Parquet: COCO avg=7.524ms（可用），Deep1M 跳过（10GB）
- [x] BoundFetch 参照：recall=0.812, E2E=2.378ms，avg_probe=1.745ms, avg_io=0.001ms

### B6：可选 baseline（完成部分）
- [x] Parquet random access：COCO 7.5ms（符合预期差），作反面教材

### Baseline 阶段输出 ✅
- [x] results/vector_search.csv（FAISS/DiskANN）
- [x] results/payload_retrieval.csv（FlatStor/Lance/Parquet）
- [x] results/e2e_comparison.csv（71 rows，各系统组合）
- [x] results/analysis.md（全量分析与结论）
- [x] **决策闸门**：FAISS+FlatStor E2E=0.181ms @ recall=0.989 vs BoundFetch E2E=2.378ms @ recall=0.812（不同配置）；BoundFetch 的优势在于 I/O-compute 重叠（io_wait=0.001ms），但纯向量搜索部分 BoundFetch 还需进一步优化

---

## 第 1 阶段：快速验证（目标：1 周）

### E6-快速：top_k 敏感性补充（Deep1M）
- [x] top_k=10: 已有数据
- [x] top_k=20: 已有数据
- [ ] top_k=50: 运行并收集 SafeOut/早停/延迟
- [ ] top_k=100: 运行并收集

### E1-快速：COCO 上的端到端
- [ ] 运行 BoundFetch: top_k={10,20,50}, nprobe={32,64,128}
- [ ] 运行 Eager-Fetch 对比
- [ ] 收集 I/O 分解统计

### E3-快速：分类消融（Deep1M）
- [ ] 完整三类 vs 仅 SafeOut vs 无分类 vs 贪心
- [ ] 收集 I/O 字节和延迟对比
- [ ] **闸门**：SafeOut ≥30% I/O 减少？

---

## 第 2 阶段：真实数据 + 核心消融（目标：2-3 周）

### 数据集准备
- [ ] 下载 MS MARCO Passage embeddings
- [ ] 准备 Amazon Products embeddings
- [ ] 构建各数据集的 BoundFetch 索引

### E1：主要结果
- [ ] MS MARCO: top_k={10,20,50,100}, nprobe={16,32,64,128}
- [ ] COCO 330K: 同上
- [ ] Deep10M (10KB合成): 同上

### E4：布局消融
- [ ] 实现分离布局变体
- [ ] 真实数据集上 3 种布局对比

### E5：多阶段消融
- [ ] 仅 1-bit vs 1+2 bit vs 1+4 bit

### E6：CRC + top_k 完整版
- [ ] top_k={10,20,50,100}, CRC on/off

### E8：SafeIn 激活条件探索
- [ ] bits={1,2,4,8} × top_k={10,50,100,200}
- [ ] d_k 校准分位数 {P90, P95, P99}

### E2：重叠分析
- [ ] prefetch_depth={1,2,4,8,16,32}

---

## 第 3 阶段：Baseline 比较（目标：2 周）

### E7：正式 baseline 比较
- [ ] 使用第 0 阶段建立的 baseline
- [ ] MS MARCO, COCO, Deep10M 上 search+payload 端到端
- [ ] 生成 Pareto 曲线

---

## 第 4 阶段：规模与润色（目标：1 周）

### E9：可扩展性
- [ ] Deep1M → Deep10M → NQ 21M

---

## 论文图表清单
- [ ] 图 1：系统架构图
- [ ] 图 2：Recall-QPS Pareto 曲线（search+payload, E7）
- [ ] 图 3：延迟分解堆叠柱（E2）
- [ ] 图 4：SafeOut 剪枝价值柱图（E3）
- [ ] 图 5：top_k 敏感性双 Y 轴图（E6）
- [ ] 图 6：SafeIn 热力图（E8, 可选）
- [ ] 图 7：可扩展性曲线（E9）
- [ ] 表 1：主要结果（真实 payload）
- [ ] 表 2：I/O 分解
- [ ] 表 3：消融总结
- [ ] 表 4：top_k 敏感性
- [ ] 表 5：Baseline 端到端对比
