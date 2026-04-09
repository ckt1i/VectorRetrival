## 0. 基线采集（所有 Phase 前共用基线）

- [ ] 0.1 记录 `bench_vector_search` Deep1M 基线：
  <!-- BASELINE bench_vector_search Deep1M: Phase B=19047ms, CRC=1531ms, recall@10=0.9685, lamhat=0.132548, eps_ip=0.151921, eps_ip_fs=0.172611, d_k=0.0588 -->
- [ ] 0.2 记录 `bench_e2e` COCO 1K 基线（关键！Phase 1-3 的验证目标）：
  ```bash
  cd /home/zcq/VDB/VectorRetrival/build/benchmarks
  ./bench_e2e --dataset /home/zcq/VDB/data/coco_1k --bits 4 --crc 1
  ```
  记录：`recall@{1,5,10}`, `avg_query_ms`, `p50/p95/p99`, `avg_io_wait`, `avg_cpu`, `avg_probe`, `safe_in/safe_out/uncertain`, `overlap_ratio`, `lamhat`, `eps_ip`, `d_k`
  <!-- BASELINE bench_e2e COCO 1K: recall@10=?, avg_ms=?, avg_cpu=?, overlap_ratio=?, eps_ip=?, d_k=? -->
- [ ] 0.3 保存 COCO 1K 的 build 目录副本（供 Phase 4 的 `--index-dir` 测试用）
- [ ] 0.4 记录 `bench_vector_search` COCO 100K 基线

## 0.5 备份 bench_vector_search.cpp 作为对照组（Option B）

- [x] 0.5.1 复制 `benchmarks/bench_vector_search.cpp` 到 `benchmarks/bench_vector_search_inline.cpp`
- [x] 0.5.2 在 `benchmarks/CMakeLists.txt` 注册新 target `bench_vector_search_inline`：
  - 源文件 `bench_vector_search_inline.cpp`
  - 链接和原 `bench_vector_search` 完全一致的库
  - OpenMP 条件链接保持一致
- [ ] 0.5.3 编译通过，运行 `bench_vector_search_inline` 和 `bench_vector_search` 应产生完全相同的数值（sanity check）
  <!-- 编译通过 ✓（仅一个 pre-existing unused-variable warning），运行 sanity check 待执行 -->
- [x] 0.5.4 在 inline 文件顶部注释写明冻结版原则
- [ ] 0.5.5 使用 `bench_vector_search_inline` 重新跑一次 Phase 0.1/0.4 基线，确认数值一致

---

## Phase 1 — 库的向量侧算法升级（无格式改动）

### 1.1 IvfBuilder 的 eps 校准改用 eps_ip_fs 公式

- [ ] 1.1.1 在 `src/index/ivf_builder.cpp` 添加 `#include "vdb/simd/signed_dot.h"` 和 OMP 守卫
- [ ] 1.1.2 在 `IvfBuilder::Build` 内部调整 phase 顺序：先 `CalibrateDk`，再 eps 校准，再 FastScan packing
  - 如果 FastScan blocks 要先 pack（因为 eps_ip_fs 需要它），把 packing 提前到 dk 之后
- [ ] 1.1.3 新增私有方法 `CalibrateEpsIpFastScan`（参考 `benchmarks/bench_vector_search.cpp:569-666`）：
  - 对每个 cluster 取 `min(100, cluster_size)` pseudo-query
  - 用 `EstimateDistanceFastScan` 得 `fs_dist`，用 `L2Sqr` 得 true_dist
  - 归一化 `|fs_dist - true| / (2 * norm_oc * norm_qc)`
  - 截断范围 `[0.1 * d_k, 10 * d_k]`
  - OMP `parallel for schedule(dynamic, 16)` + thread-local error vectors
  - P99 作为 `calibrated_eps_ip_`
- [ ] 1.1.4 删除旧的 popcount 版 eps_ip 校准代码
- [ ] 1.1.5 编译通过，单元测试通过

### 1.2 IvfIndex 即时计算 rotated_centroids

- [ ] 1.2.1 在 `include/vdb/index/ivf_index.h` 添加私有字段：
  ```cpp
  std::vector<float> rotated_centroids_;  // nlist × dim，Hadamard 时填充
  bool used_hadamard_ = false;
  ```
- [ ] 1.2.2 添加 public accessor：
  ```cpp
  bool used_hadamard() const { return used_hadamard_; }
  const float* rotated_centroid(uint32_t k) const;
  ```
- [ ] 1.2.3 `IvfIndex::Open` 加载完 rotation 后，检测是否 Hadamard（`dim` 是 2 的幂 + rotation 来源），如果是就预计算 `rotated_centroids_`
- [ ] 1.2.4 为 OverlapScheduler 暴露 `used_hadamard()` 和 `rotated_centroid()`

### 1.3 OverlapScheduler 使用 PrepareQueryRotatedInto 快路径

- [ ] 1.3.1 在 `OverlapScheduler::Search` 开头，如果 `index_.used_hadamard()`，一次性旋转 query：
  ```cpp
  if (index_.used_hadamard()) {
      rotation.Apply(query, rotated_q_.data());
  }
  ```
- [ ] 1.3.2 `ProbeCluster` 里：Hadamard 路径用 `PrepareQueryRotatedInto(rotated_q, rotated_c[cid], &pq)`，否则走原来的 `PrepareQuery`
- [ ] 1.3.3 `pq` 从每次 `PrepareQuery` 改为 `PrepareQueryInto`，复用 scheduler 持有的 `PreparedQuery` 对象

### 1.4 删除 popcount 死代码（注意 inline bench 对照组）

- [ ] 1.4.1 在 `src/index/` 和 `src/query/` 中 grep `PopcountXor|hamming` 找出所有 popcount 调用
- [ ] 1.4.2 检查 `bench_vector_search_inline.cpp`（Phase 0.5 备份）是否还在使用 `RaBitQEstimator::EstimateDistance` popcount 版本
  - 如果是：**保留该 API**，标记 `[[deprecated]]`；新代码不调用，但 inline bench 能继续编译
  - 如果否：直接删除
- [ ] 1.4.3 **不修改** `bench_vector_search_inline.cpp` 的 popcount 对照路径（inline 版是冻结版）
- [ ] 1.4.4 删除 `bench_vector_search.cpp`（原始未备份前的版本不存在 popcount 删除——Phase 4 才重写这个文件）
- [ ] 1.4.5 保留 `simd::PopcountXor` kernel（可能被 CRC 校准用到，grep 确认）
- [ ] 1.4.6 编译 `bench_vector_search_inline` 确认未破坏对照组

### 1.5 Phase 1 验证

- [ ] 1.5.1 `ctest --output-on-failure`：pre-existing 2 个 failures 不变，其他均通过
- [ ] 1.5.2 bench_vector_search Deep1M：数值和基线一致（recall, lamhat, eps_ip_fs, d_k）
- [ ] 1.5.3 bench_e2e COCO 1K：**recall@10 应不变或提升**（eps_ip_fs 更紧导致 SafeOut 更多，但因为用的是 ConANN adaptive，不应丢 recall）
  <!-- 观察：SafeOut 上升多少？CPU 时间降低多少？ -->
- [ ] 1.5.4 bench_e2e 的 `avg_cpu` 和 `avg_probe` 应下降（预期 20-40%）
- [ ] 1.5.5 记录 Phase 1 后数值：
  <!-- Phase 1 after: bench_e2e COCO 1K recall@10=?, avg_cpu=?, overlap_ratio=? -->

---

## Phase 2 — 段级格式升级（`rotated_centroids.bin` + `eps_ip_fs` 持久化）

### 2.1 新增 rotated_centroids 磁盘存储

- [ ] 2.1.1 `IvfBuilder::WriteIndex` 在 Hadamard 路径下把 `rotated_centroids` 写入 `<output_dir>/rotated_centroids.bin`（nlist × dim × f32 raw）
- [ ] 2.1.2 `IvfIndex::Open` 优先从 `rotated_centroids.bin` 读取，读不到则按 Phase 1.2.3 即时计算（过渡兼容）
- [ ] 2.1.3 删除 Phase 1.2.3 的即时计算代码（Open 时必须从磁盘读）

### 2.2 segment.meta 新增 eps_ip_fs 字段

- [ ] 2.2.1 找到 `segment.fbs`（FlatBuffers schema），添加字段：
  ```
  eps_ip_fs: float;
  has_rotated_centroids: bool = false;
  ```
- [ ] 2.2.2 重新生成 C++ FlatBuffers 代码，编译通过
- [ ] 2.2.3 `IvfBuilder::WriteIndex` 写入 `eps_ip_fs` 和 `has_rotated_centroids = true`
- [ ] 2.2.4 `IvfIndex::Open` 读取 `eps_ip_fs` 并存入 `conann_`（ConANN 构造参数）
- [ ] 2.2.5 如果 `has_rotated_centroids == false` → 直接报错（不兼容 v7）

### 2.3 Phase 2 验证

- [ ] 2.3.1 重建 COCO 1K 索引（新格式）
- [ ] 2.3.2 `bench_e2e` 运行正常，数值与 Phase 1.5.3 一致
- [ ] 2.3.3 尝试用旧 .clu v7 目录运行 → 应该报"missing rotated_centroids.bin"
- [ ] 2.3.4 记录 Phase 2 后数值（应该和 Phase 1 完全一致，只是格式变了）

---

## Phase 3 — ClusterProber 抽取

### 3.1 定义接口和类

- [ ] 3.1.1 新增 `include/vdb/index/cluster_prober.h`：
  ```cpp
  namespace vdb::index {

  // SafeOut lanes 由 ClusterProber 内部消化（stats.s1_safeout++）
  // 传给 Sink 的只有 SafeIn 和 Uncertain 两类
  enum class CandidateClass { SafeIn, Uncertain };

  class ProbeResultSink {
  public:
      virtual ~ProbeResultSink() = default;
      virtual void OnCandidate(uint32_t vec_idx, AddressEntry addr,
                               float est_dist, CandidateClass cls) = 0;
  };

  struct ProbeStats {
      uint32_t s1_safein = 0, s1_safeout = 0, s1_uncertain = 0;
      uint32_t s2_safein = 0, s2_safeout = 0, s2_uncertain = 0;
  };

  class ClusterProber {
  public:
      ClusterProber(const ConANN& conann, Dim dim, uint8_t bits);

      // dynamic_d_k: 由 scheduler 每次调用前算好
      //   (use_crc && est_heap.size() >= top_k) ? est_heap.front().first : conann.d_k()
      void Probe(const query::ParsedCluster& pc,
                 const rabitq::PreparedQuery& pq,
                 float margin_factor,
                 float dynamic_d_k,
                 ProbeResultSink& sink,
                 ProbeStats& stats) const;
  private:
      const ConANN& conann_;
      Dim dim_;
      uint8_t bits_;
      float margin_s2_divisor_;  // 2^(bits-1)
  };

  }  // namespace vdb::index
  ```
  **不实现** `GetDynamicDk` 方法（Decision 4b：scheduler 持有 est_heap，算好后作为 float 参数传入）。

### 3.2 实现 ClusterProber::Probe

- [ ] 3.2.1 新增 `src/index/cluster_prober.cpp`，实现 `Probe`：
  - 复用 bench_vector_search FastScan path 的逻辑（L883-1003）
  - 对每个 block：调 `EstimateDistanceFastScan` → `FastScanSafeOutMask` → ctz 迭代
  - Stage 1 分类：`ClassifyAdaptive(est_dist, margin_s1, est_kth)`（est_kth 从 sink 获取？→ 简化：只做 `Classify(est_dist, margin_s1)`，由 sink 负责 est_kth）
  - 非-SafeOut lane 再走 Stage 2 `IPExRaBitQ`（bits > 1）
  - 非-SafeOut 最终 lane 调 `sink.OnCandidate()`
- [ ] 3.2.2 `ParsedCluster` 缺 `margin_factor` 的 r_max？用 `pc.epsilon` 字段（已存在）

### 3.3 AsyncIOSink 封装 PrepRead

- [ ] 3.3.1 在 `src/query/overlap_scheduler.cpp` 内部定义 `AsyncIOSink : public ProbeResultSink`
  - 构造时持有 `OverlapScheduler&` / `SearchContext&` / `RerankConsumer&` 的引用
- [ ] 3.3.2 `OnCandidate(vec_idx, addr, est_dist, cls)` 实现：
  - `cls == SafeIn` + `addr.size <= safein_all_threshold` → `PrepRead(VEC_ALL)`
  - `cls == SafeIn` + `addr.size > threshold` → `PrepRead(VEC_ONLY)`
  - `cls == Uncertain` → `PrepRead(VEC_ONLY)`
  - 对应 `buffer_pool_.Acquire` + `pending_[buf] = {...}` 填充
- [ ] 3.3.3 Sink **不持有 est_heap**（Decision 4b：est_heap 留在 scheduler）

### 3.4 ProbeCluster 变薄

- [ ] 3.4.1 `OverlapScheduler` 新增成员：
  - `ClusterProber prober_`（构造时初始化）
  - `std::vector<float> rotated_q_`（重用的 query rotation buffer）
  - `PreparedQuery pq_`（重用的 per-cluster PreparedQuery）
- [ ] 3.4.2 在 `Search()` 开头（不是 ProbeCluster 内部）一次性算 `rotated_q_`（Hadamard 路径）
- [ ] 3.4.3 把 `OverlapScheduler::ProbeCluster` 改为：
  ```cpp
  // Hadamard 快路径复用 rotated_centroids
  if (index_.used_hadamard()) {
      estimator.PrepareQueryRotatedInto(
          rotated_q_.data(), index_.rotated_centroid(cid), &pq_);
  } else {
      estimator.PrepareQueryInto(
          ctx.query_vec(), index_.centroid(cid), index_.rotation(), &pq_);
  }
  float margin_factor = 2.0f * pq_.norm_qc * index_.conann().epsilon();

  // dynamic_d_k 由 scheduler 持有 est_heap 算出
  float dynamic_d_k = (use_crc_ && est_heap_.size() >= est_top_k_)
      ? est_heap_.front().first : index_.conann().d_k();

  AsyncIOSink sink(*this, ctx, reranker);
  ProbeStats local_stats;
  prober_.Probe(pc, pq_, margin_factor, dynamic_d_k, sink, local_stats);

  // 聚合 stats
  ctx.stats().total_safe_in   += local_stats.s1_safein;
  ctx.stats().total_safe_out  += local_stats.s1_safeout;
  ctx.stats().total_uncertain += local_stats.s1_uncertain;
  ctx.stats().s2_safe_in      += local_stats.s2_safein;
  ctx.stats().s2_safe_out     += local_stats.s2_safeout;
  ctx.stats().s2_uncertain    += local_stats.s2_uncertain;
  ```
- [ ] 3.4.4 **est_heap 维护逻辑**：Prober 在 `Probe()` 内部不更新 est_heap（它没有 est_heap 的所有权）。scheduler 在调完 Probe 之后，需要从 sink 里拿到所有「非-SafeOut 候选的 est_dist」来更新 est_heap。
  - 简化方案：让 `AsyncIOSink::OnCandidate` 同时更新 scheduler 的 `est_heap_`（通过持有的 scheduler 引用）
  - 这样 Sink 实际上同时管 I/O 提交和 est_heap 维护；est_heap 的**所有权**还在 scheduler，但 Sink 有更新权限

### 3.5 Phase 3 验证（最关键的 bit-exact 验证）

- [ ] 3.5.1 bench_e2e COCO 1K：`recall@{1,5,10}` 与 Phase 2.3.4 **bit-exact 相同**
- [ ] 3.5.2 `avg_probed`、`safe_in/out/uncertain` **bit-exact 相同**
- [ ] 3.5.3 如果数值漂移：检查 FastScan mask 和 scalar classify 的浮点一致性
- [ ] 3.5.4 `avg_cpu` 应进一步下降（FastScanSafeOutMask batch 比 scalar 快）
- [ ] 3.5.5 `avg_io_wait` 应下降（更多 SafeOut 跳过 I/O）
- [ ] 3.5.6 记录 Phase 3 后数值：
  <!-- Phase 3 after: bench_e2e COCO 1K avg_cpu=?, avg_io_wait=?, overlap_ratio=? -->

---

## Phase 4 — bench 统一到磁盘路径 + 纯查询模式

### 4.1 bench_e2e 增加 --index-dir 参数

- [ ] 4.1.1 `bench_e2e.cpp` 解析 `--index-dir`
- [ ] 4.1.2 如果指定：跳过 Phase A（仍需 load queries 用于搜索 + 载 gt）+ 跳过 Phase C (Build)，直接 `index.Open(index_dir)`
- [ ] 4.1.3 Phase C.5 (CRC calibration)：优先从 `index_dir/crc_scores.bin` 加载
- [ ] 4.1.4 验证：先用旧方式 build 一个 COCO 1K 索引到 `/tmp/coco1k_idx`，再用 `--index-dir /tmp/coco1k_idx` 运行，数值完全一致

### 4.0 Spike：确认 payload_fn=nullptr / empty payload_schemas 路径可用

- [ ] 4.0.1 读 `src/index/ivf_builder.cpp`，确认 `IvfBuilderConfig::payload_schemas` 为空且 `payload_fn=nullptr` 时 `.data` 只含向量且 `IvfBuilder::Build` 不报错
- [ ] 4.0.2 读 `src/storage/segment.cpp` / `src/storage/data_file.cpp`，确认 `DataFileReader` 在无 payload schema 时能正常读取
- [ ] 4.0.3 读 `src/query/rerank_consumer.cpp`，确认 `ConsumeVec` / `ConsumeAll` 在无 payload 时行为正确（VEC_ONLY 路径应该天然工作；VEC_ALL 路径等价于 VEC_ONLY）
- [ ] 4.0.4 写一个最小 spike 测试：生成 1000 个 Deep1M 风格的随机向量，调 `IvfBuilder::Build(payload_schemas={}, payload_fn=nullptr)`，Open，Query 一个已知向量
  - 如果不能工作：补最小修复（只修"至少一个 payload 列"的硬编码假设，不做新功能）
- [ ] 4.0.5 记录发现到 Phase 4.0 的注释里（供 Phase 4.2 参考）

### 4.2 bench_vector_search 重写（不含 inline 版）

**前置**：`bench_vector_search_inline.cpp`（Phase 0.5 备份）保持原样。本任务只重写 `bench_vector_search.cpp`。

- [ ] 4.2.1 删除 `bench_vector_search.cpp` 的 inline KMeans/Encode/calibrate/search 代码（L349-1115）
- [ ] 4.2.2 保留：CLI、Phase A（load .fvecs）、Phase E（recall）、Phase F（stats/JSON）
- [ ] 4.2.3 新增 Phase B：`IvfBuilder::Build` 构建到临时目录，`payload_schemas={}` + `payload_fn=nullptr`
  - 临时目录：`--tmp-dir` 参数（默认 `/tmp/bench_vs_<timestamp>`）
  - 测试结束自动清理（除非 `--keep-index`）
- [ ] 4.2.4 新增 Phase C：`IvfIndex::Open` + `OverlapScheduler::Search` 循环
  - 循环结构和 bench_e2e 的 `RunQueryRound` 一致
  - 用 `IoUringReader` 读 .data（验证走的是磁盘路径）
- [ ] 4.2.5 用 `strace -c -e read,pread64,io_uring_enter` 或 `perf stat -e syscalls:sys_enter_read` 验证确实发生了磁盘 I/O（不是 mmap 零开销）
- [ ] 4.2.6 两边数值对比：新 `bench_vector_search` Deep1M recall@10 应等于 `bench_vector_search_inline` 的 baseline

### 4.3 bench_vector_search 也支持 --index-dir

- [ ] 4.3.1 添加参数解析
- [ ] 4.3.2 跳过 Phase B，直接 Open

### 4.4 统一 build-or-load helper（可选）

- [ ] 4.4.1 如果两边的 "build-or-load" 逻辑重复太多，抽成 `benchmarks/common/build_or_load.h`
- [ ] 4.4.2 否则就 copy-paste（~30 行）

### 4.5 Phase 4 验证

- [ ] 4.5.1 bench_vector_search Deep1M：数值和 Phase 0 基线一致
  - `recall@10` = 0.9685
  - `lamhat` 相对误差 < 0.1%
- [ ] 4.5.2 bench_vector_search COCO 100K：数值和 Phase 0 基线一致
- [ ] 4.5.3 验证 bench_vector_search 现在确实从磁盘读 .clu（用 `strace` 或 `perf stat` 验证 I/O 系统调用）
- [ ] 4.5.4 测量两边加 `--index-dir` 之后的启动时间（应该只剩 query 时间）

---

## 5. 最终验证 + 清理

- [ ] 5.1 完整跑一遍测试套件：
  - `bench_vector_search` Deep1M + COCO 100K（新薄 bench，走磁盘）
  - `bench_vector_search_inline` Deep1M + COCO 100K（对照组，如果还能编）
  - `bench_e2e` COCO 1K
- [ ] 5.2 `ctest --output-on-failure`：确认只有 2 个 pre-existing failures
- [ ] 5.3 代码行数 diff：
  <!-- bench_vector_search: 1261 → ? lines -->
  <!-- bench_vector_search_inline: 1261 → ? lines (保持，仅最小修复) -->
- [ ] 5.4 性能总结：
  <!-- bench_e2e COCO 1K: avg_cpu 基线 ? → 最终 ?, avg_io_wait 基线 ? → 最终 ?, recall 不变 -->
  <!-- bench_vector_search vs bench_vector_search_inline: recall 一致，延迟差异多少？ -->
- [ ] 5.5 更新 `.claude/memory/` 中的 project_phaseb_optimization.md
- [ ] 5.6 归档 `.clu v7` 相关死代码（如果有的话）
- [ ] 5.7 确认 `bench_vector_search_inline` 的状态（还在编 / 编译被 API 破坏 / 已废弃）记录到 memory

---

## 6. 未来工作（Non-Goals，仅记录）

- [ ] 6.1 per-block-32 粒度的 CPU-I/O 流水优化（`probe-io-block32-interleaved`）
- [ ] 6.2 `FlatBuffers segment.meta v9` 真正重整（目前是增量加字段）
- [ ] 6.3 去掉 `IoUringReader` 的 buffer_pool 分配热点（Phase 3 后可能成为新瓶颈）
- [ ] 6.4 `ClusterProber` 支持 batch-multiple-cluster 并行（SIMD 跨 cluster）
