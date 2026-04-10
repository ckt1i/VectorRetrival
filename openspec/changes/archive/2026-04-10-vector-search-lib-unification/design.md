## Context

四个阶段的代码触及面不同，但有一条贯穿全程的设计主线：**`ParsedCluster` 作为两条搜索路径的统一数据结构**。这是 Phase 2/3 能够干净合并的基础。

## Decisions

### Decision 1: `ParsedCluster` 作为统一 cluster 表示

**问题**：bench_vector_search 目前用 `std::vector<std::vector<FsBlock>> cluster_fs_blocks[nlist]`，overlap_scheduler 用 `ParsedCluster`（零拷贝进 I/O buffer）。`ClusterProber::Probe` 只能接一种。

**选择**：统一用 `ParsedCluster`（v8 layout）。bench_vector_search 在 Phase 4 里也走磁盘路径（这正是用户的新要求），所以两边的 `ParsedCluster` 都来自 `OverlapScheduler` 读取的 .clu v8 buffer，**不需要在内存里"假装构建" ParsedCluster**——这个旧顾虑因为 Phase 4 的磁盘化自动消失。

### Decision 2: `eps_ip_fs` 如何替换 `eps_ip`

**问题**：`IvfBuilder` 的 `calibrated_eps_ip_` 是 popcount 版的 IP error，公式不同。

**方案**：
- **保留字段名** `calibrated_eps_ip_`，语义改为 FastScan 归一化 distance error。
- 需要 d_k 先算出来才能做截断 → 调整 IvfBuilder 内部相位：先 `CalibrateDk`，再 `CalibrateEpsIpFs`。
- 采样策略：和 bench_vector_search 一致，每个 cluster 取 `min(100, cluster_size)` 个 pseudo-query，计算所有 target（同 cluster 内）的 FastScan dist error，用 `|fs_dist - true_L2| / (2 * norm_oc * norm_qc)` 归一化。
- **截断范围** `[0.1 * d_k, 10 * d_k]`：和 bench_vector_search 一致。
- 采用 OMP `parallel for schedule(dynamic, 16)` + thread-local error vectors（复用 eps-calibration-simd 的模板）。

**跨模态 query 支持**：`IvfBuilderConfig::calibration_queries` 已经在用于 d_k，eps_ip_fs 继续沿用："有 query 用 query，没 query 用 base 采样"。

### Decision 3: Phase 1 不改格式如何让 IvfIndex 拿到 `rotated_centroids`？

**问题**：Phase 1 要让 `OverlapScheduler` 用 `rotated_centroids` + `PrepareQueryRotatedInto`，但 .clu v8 要到 Phase 2 才落地。

**方案**：`IvfIndex::Open` 读完 `rotation.bin` 和 `centroids.bin` 后**在内存里即时计算** `rotated_centroids`。开销一次性（nlist × O(dim log dim) Hadamard），对 Phase 1 测试可忽略。Phase 2 落地后此路径被"改从 .clu 读"替换。

### Decision 4: `ClusterProber` 接口设计

**背景**：bench_vector_search 改成磁盘路径后，两条 bench 都通过 `OverlapScheduler` 走 io_uring，**Sink 的生产实现只有一个** (`AsyncIOSink`)。这让我们重新评估 Sink 抽象是否还值得保留。

**结论：保留 Sink 抽象，但只实现一个生产 Sink**。原因：

1. 虚函数开销（~1 ns）相对 `PrepRead` syscall 可忽略
2. 保留 `ClusterProber` 作为纯 CPU 组件，不依赖 `io_uring_reader.h` / `buffer_pool.h`，编译依赖干净
3. 为未来的 per-block CPU-I/O 流水优化（独立 change）预留插入点——那个改动的实质是换一个新 Sink 行为，不触动 `ClusterProber`
4. 单元测试可以写 `MockSink` 只记录候选，不经过 io_uring

**实施阶段性策略**（用户确认）：**Phase 3 先直接实现 `AsyncIOSink`**，不做 `MockSink` 或额外 Sink 类型。Sink 接口本身是简单的 virtual，追加实现成本为零。

**`ProbeResultSink` 接口**：

```cpp
class ProbeResultSink {
public:
    virtual ~ProbeResultSink() = default;

    // 经过 Stage 1/2 分类后非-SafeOut 的候选，交给 sink 决定如何 rerank
    virtual void OnCandidate(uint32_t vec_idx_in_cluster,
                             AddressEntry addr,
                             float est_dist,
                             ResultClass classification) = 0;
};
```

**`ClusterProber::Probe` 签名**（`est_kth` 由 scheduler 持有并作为参数传入 — Decision 4b）：

```cpp
void Probe(const query::ParsedCluster& pc,
           const rabitq::PreparedQuery& pq,
           float margin_factor,          // 2 * pq.norm_qc * eps_ip_fs
           float dynamic_d_k,             // scheduler 每 cluster 前算好传入
           ProbeResultSink& sink,
           ProbeStats& stats) const;
```

### Decision 4b: `est_kth` / `dynamic_d_k` 的持有方

**问题**：CRC early-stop 模式下，`ClassifyAdaptive` 需要动态的 `est_kth`（= est_heap 的 top），随着 probe 进度变化。

**选择**：**由 scheduler 持有 `est_heap`**，每次调 `Probe` 之前计算 `dynamic_d_k`，作为参数传入。**不把 est_heap 塞进 Sink**。

**理由**：
- `est_heap` 是 query 级状态，跨 cluster 共享；Sink 是 I/O 提交的薄封装，把 query 状态塞进去会让 Sink 重型化
- Scheduler 本来就要维护 est_heap（CRC stopper 的 `ShouldStop` 判断依赖它）
- Probe 签名多一个 float 参数的代价远小于 Sink 内部状态复杂化的代价

**`AsyncIOSink`（query/overlap_scheduler 用）**：封装 `PrepRead` + buffer_pool acquire，和当前 `OverlapScheduler::ProbeCluster` 的尾部逻辑（L235-314，约 80 行）一一对应。

### Decision 5: .clu v8 格式

现有 .clu v7 layout（per-cluster block）：
```
[header][Region 1: FastScan blocks][Region 2: ExRaBitQ entries][Region 3: address packed]
```

v8 改动（segment-global，不在 per-cluster block 里）：
- 新增 `rotation.bin` 数据 → 集成为 segment-global 区域
- 新增 `rotated_centroids` → 也放 segment-global 区域

**选择**（用户已确认）：**不改 per-cluster block 格式**，而是在 segment level 新增一个 `rotated_centroids.bin`。完整方案：
- `centroids.bin` → 保持不变（raw centroids）
- `rotation.bin` → 保持不变（rotation matrix）
- **新增** `rotated_centroids.bin`（nlist × dim × f32，raw）
- `segment.meta` 里加 bool `has_rotated_centroids` + float `eps_ip_fs`

.clu v7 的 per-cluster block 格式不变，`ParsedCluster` 解析器无需改动。

**不保留 v7 兼容**：`IvfIndex::Open` 检测不到 `has_rotated_centroids = true` 直接报错（开发阶段重建）。

### Decision 6: bench_vector_search 重写范围

删除的代码（约 700 行）：
- inline KMeans, Encode, cluster_fs_blocks 构造（L393-532）
- 所有 eps_ip / eps_ip_fs / d_k / CRC inline 校准（L417-759）
- SOA layout 构造（L762-782）
- 主搜索循环（L826-1109）
- popcount 路径（L1004-1076）

保留的代码：
- CLI 解析
- Phase A（载入 base/query/gt）
- Phase E（召回率计算）
- Phase F（统计打印 + JSON）

新增的代码（约 150 行）：
- `IvfBuilder::Build` 调用 + 写临时目录
- `IvfIndex::Open` + `OverlapScheduler` 查询循环
- `--index-dir` 参数支持（跳过 build，直接 Open）

### Decision 6b: Deep1M / 仅向量搜索任务的 payload 处理

**用户确认**：bench_vector_search 走磁盘路径时，.data 文件**只存原始向量，不存 payload**。

**方案**：bench_vector_search 调 `IvfBuilder::Build(..., payload_fn=nullptr)`，并且 `IvfBuilderConfig::payload_schemas` 设为空 `{}`。需要验证：
- `IvfBuilder` 支持 empty payload_schemas（当前代码路径是否已支持？Phase 4.0 的 spike 任务确认）
- `IvfIndex::Open` 支持 `payload_schemas.empty()`
- `OverlapScheduler::Search` 在 `payload_schemas` 为空时不生成 PAYLOAD 读请求

如果代码路径未支持 empty payload，在 Phase 4 里补足最小实现——**不做新功能**，只把现有假设的「至少有一个 payload 列」的地方改为可选。

bench_e2e 走 COCO 数据集时仍然有 payload（image_id + caption），两条路径在数据文件布局上的区别只在于「是否含 payload 列」。

### Decision 7: `--index-dir` 参数语义

两个 bench 行为一致：
- **未指定 `--index-dir`**：和当前一致。构建到临时目录（可选 `--save-index` 保留），测完清理。
- **指定 `--index-dir`**：跳过 Phase B/C（build + CRC 校准从 precomputed scores.bin 加载），直接 Open + query。

这解决"每次换 query 参数都要重 build"的痛点。

### Decision 8: bench_vector_search 备份策略

**用户确认：Option B — 作为第二个 bench target 编译并维护**。

具体做法：
- Phase 0.5 把现有 `benchmarks/bench_vector_search.cpp` 复制为 `benchmarks/bench_vector_search_inline.cpp`
- 在 `benchmarks/CMakeLists.txt` 注册新 target `bench_vector_search_inline`，链接同样的库
- 新 target 保持「三轮优化冻结版」，Phase 1-4 的库改动**可能**让它继续编译（如果 API 兼容）或者**不再编译**（如果 API 破坏）
- 明确原则：**inline 版是 frozen baseline，不主动维护**；如果库 API 破坏导致它无法编译，只做最小编译修复；如果修复代价过大，标记为废弃而不是同步更新
- Phase 4 重写原 `bench_vector_search.cpp` 后，两个 bench 并存：
  - `bench_vector_search`（薄 bench，走库路径）
  - `bench_vector_search_inline`（原 inline 版，对照组）

**副作用**：Phase 1 删除 popcount 死代码时，如果 `RaBitQEstimator::EstimateDistance` 被 inline 版用到，需要保留这个 API（标记 `[[deprecated]]`）或者允许 inline 版编译失败。取决于具体情况在 Phase 1.4 决定。

## Risks / Trade-offs

### R1: [Phase 3 数值漂移风险] ⚠️ 最高风险
`ClusterProber` 把 bench_vector_search 的 FastScan batch-mask 逻辑搬到 `OverlapScheduler`，虽然数学相同，但浮点运算顺序的微小差异可能导致 `bench_e2e` 的 recall 数字轻微变化（±0.001）。

**缓解**：Phase 3 的验证用 bit-exact 对比——在实现前先记录 bench_e2e 基线 `recall@10/5/1`、`lamhat`、`avg_probed`，改完后必须完全一致。

### R2: [Phase 1 的 eps_ip_fs 在 IvfBuilder 里必须拿到 cluster_fs_blocks]
IvfBuilder 目前不构建 FastScan blocks（那是 writer 阶段做的）。Phase 1 需要把 FastScan packing 提前到 calibrate 之前，或者在 calibrate 里现场 pack。

**选择**：提前 pack（和 bench_vector_search 的顺序一致）。代码增量约 30 行。

### R3: [Phase 4 bench_vector_search 的内存爆炸]
原来 bench 在内存里的 .fvecs 数据不落盘。改成磁盘路径后，需要把 base vectors 写进 .dat 文件——Deep1M 数据集约 400 MB，COCO 100K 约 280 MB。

**缓解**：把 build 的临时目录放在 `/tmp` 或者用户指定路径（`--tmp-dir`），测完清理。

### R4: [Phase 2 的 segment.meta FlatBuffers schema 兼容]
新增 `has_rotated_centroids` 字段需要改 `segment.fbs`。FlatBuffers 支持添加字段（向前兼容），但需要重新生成 C++ 代码。

**缓解**：Phase 2 开头验证 FlatBuffers 代码生成链路。

### R5: [OverlapScheduler 的 per-cluster `RaBitQEstimator` 构造成本]
现在 `ProbeCluster` 每次调用都 `RaBitQEstimator estimator(dim)`。Phase 3 把它移到 scheduler 构造时（ClusterProber 可以无状态或持有对 estimator 的引用）。

**缓解**：这是顺手的优化，风险低。

## Migration / Rollback 策略

每个 Phase 完成后都有一个 "git-safe" 点，bench_e2e 能跑。Phase 顺序不能打乱，但任何 Phase 出问题都可以回滚到上一阶段。

| Phase | 回滚影响 |
|-------|---------|
| Phase 1 回滚 | bench_e2e 恢复旧 eps_ip，性能回退 |
| Phase 2 回滚 | bench_e2e 从 .clu v7 读取，运行时计算 rotated_centroids |
| Phase 3 回滚 | `ClusterProber` 回退为 scalar loop（保留代码分支一段时间） |
| Phase 4 回滚 | bench_vector_search 恢复 inline 路径，独立工作 |
