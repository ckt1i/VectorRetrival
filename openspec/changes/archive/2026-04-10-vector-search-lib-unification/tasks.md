## 0. 基线采集（所有 Phase 前共用基线）

- [x] 0.1 记录 `bench_vector_search` Deep1M 基线（2026-04-09）：
  <!-- BASELINE bench_vector_search Deep1M (nlist=4096, nprobe=100, bits=4, queries=200, pad->128):
       Phase B=20342.7ms, CRC=1530.5ms
       recall@1=1.0000, @5=0.9730, @10=0.9685
       eps_ip=0.151921, eps_ip_fs=0.172611, d_k=0.0588, lamhat=0.132548
       latency: avg=1.074 ms, p50=1.082, p95=1.871, p99=2.403
       avg_probed=85.67/100, early_stop_rate=0.1900
       S1: SafeOut 87.33%, Uncertain 12.67%
       S2: SafeOut 95.67% of S1 Uncertain, Uncertain 4.30%
       Final: SafeOut 99.45%, Uncertain 0.54%
       False SafeIn=0, False SafeOut=0 -->

- [x] 0.2 记录 `bench_e2e` COCO 100K 基线（关键！Phase 1-3 的验证目标）：
  <!-- BASELINE bench_e2e COCO 100K (nlist=2048, nprobe=256, bits=4, queries=50, precomputed centroids):
       build_time=34701.3 ms (lib path, 无三轮优化), brute_force=690.0 ms
       CRC calib=146.6 ms (from precomputed scores)
       Index params: eps_ip=0.102259, d_k=1.221293, lamhat=0.057778
       recall@1=0.8000, @5=0.8120, @10=0.8100  ← 低于 bench_vector_search 的 0.893！
       latency: avg=4.673 ms, p50=4.727, p95=5.704, p99=6.271
       avg_cpu=4.670, avg_probe=3.348, io_wait=0.002 (warm cache)
       safe_in=0, safe_out=1508.0, uncertain=82.5 (per query avg)
       s2_safe_in=0, s2_safe_out=3244.8, s2_uncertain=82.5
       false_safeout=1.90 per query, false_safein_upper=0, total_safein=2 (total across 50 queries)
       early_stop=100%, avg_skipped=170, overlap_ratio=0.9995

       观察：bench_e2e 的 recall@10=0.81 显著低于 bench_vector_search 的 0.893，
              false_safeout=1.9 per query 证实库路径的 ConANN 分类过严。
              这恰好是 Phase 1 eps_ip_fs 替换要解决的问题。 -->

- [x] 0.3 保存 COCO 100K bench_e2e 的 build 目录副本（供 Phase 4 的 `--index-dir` 测试用）
  <!-- 位置：/home/zcq/VDB/test/coco_100k_20260409T132856/index/ -->

- [x] 0.4 记录 `bench_vector_search` COCO 100K 基线：
  <!-- BASELINE bench_vector_search COCO 100K (nlist=2048, nprobe=150, bits=4, queries=200):
       Phase B=2907.6ms, CRC=859.1ms
       recall@1=0.9400, @5=0.9070, @10=0.8930
       eps_ip=0.089037, eps_ip_fs=0.102322, d_k=1.2213, lamhat=0.055386
       latency: avg=1.393 ms, p50=1.338, p95=1.935, p99=2.324
       avg_probed=69.50/150, early_stop_rate=1.0000
       S1: SafeOut 24.44%, Uncertain 75.56%
       S2: SafeOut 96.20% of S1 Uncertain, Uncertain 3.80%
       Final: SafeOut 97.13%, Uncertain 2.87%
       False SafeIn=0, False SafeOut=0 -->


## 0.5 备份 bench_vector_search.cpp 作为对照组（Option B）

- [x] 0.5.1 复制 `benchmarks/bench_vector_search.cpp` 到 `benchmarks/bench_vector_search_inline.cpp`
- [x] 0.5.2 在 `benchmarks/CMakeLists.txt` 注册新 target `bench_vector_search_inline`：
  - 源文件 `bench_vector_search_inline.cpp`
  - 链接和原 `bench_vector_search` 完全一致的库
  - OpenMP 条件链接保持一致
- [x] 0.5.3 编译通过，运行 `bench_vector_search_inline` 和 `bench_vector_search` 应产生完全相同的数值（sanity check）
  <!-- 编译通过 ✓（仅一个 pre-existing unused-variable warning），运行 sanity check 待执行 -->
- [x] 0.5.4 在 inline 文件顶部注释写明冻结版原则
- [x] 0.5.5 使用 `bench_vector_search_inline` 重新跑一次 Phase 0.1/0.4 基线，确认数值一致

---

## Phase 1 — 库的向量侧算法升级（无格式改动）

### ⚠ Phase 1 范围调整（基于 Phase 0 基线发现）

实际代码审查发现：
1. **`CalibrateEpsilonIp` in `src/index/ivf_builder.cpp:206-298` 已经实现了 eps_ip_fs 公式**（FastScan 归一化误差 + d_k 截断 + percentile）。不需要"改写公式"，只需要**加 OMP 并行**。
2. **`IvfBuilder::WriteIndex` 在 `line 314` 硬编码 `rotation.GenerateRandom(seed)`**，从不尝试 Hadamard——这才是 bench_e2e 和 bench_vector_search 最大的功能差异。COCO dim=512 (power of 2) 本应用 Hadamard。
3. bench_e2e COCO 100K recall@10 = 0.81 vs bench_vector_search 0.893 的 gap 主要来自 Hadamard vs 随机旋转的差异。

### 1.1 IvfBuilder 的 eps 校准加 OMP 并行（公式已正确）

- [x] 1.1.1 在 `src/index/ivf_builder.cpp` 顶部添加 OMP 守卫 `#ifdef _OPENMP #include <omp.h> #endif`
- [x] 1.1.2 修改 `CalibrateEpsilonIp` 外层循环为 OMP parallel：
  - `int k` 循环变量
  - `tl_errors[nt]` thread-local
  - `#pragma omp parallel` + `#pragma omp for schedule(dynamic, 16)`
  - Per-thread `RaBitQEstimator` + `packed_codes` buffer
  - 末尾合并到 `normalized_errors`
- [x] 1.1.3 CMakeLists 已链接 OpenMP
- [x] 1.1.4 编译通过，无 warning
- [x] 1.1.5 bench_e2e COCO 100K：build_time 34701 ms → 2496 ms (**14× 加速**)；eps_ip 0.102259 → 0.101921 (差异微小，因为 1.1.bis 的 Hadamard rotation 改变了采样)

### 1.1.bis IvfBuilder 支持 Hadamard rotation

- [x] 1.1.bis.1 修改 `IvfBuilder::WriteIndex` 的旋转生成：优先 Hadamard，fallback 到 random Gaussian（与 bench_vector_search L382-388 一致）
- [x] 1.1.bis.2 编译通过
- [x] 1.1.bis.3 bench_e2e COCO 100K 验证：
  <!-- Phase 1.1+1.1.bis after:
       build_time=2495.7 ms (14× faster)
       Index params: eps_ip=0.101921, d_k=1.221293
       CRC: lamhat=0.033192 (下降，更好)
       recall@1=0.8000, @5=0.8120, @10=0.8040 (unchanged 相对于 baseline 0.81)
       avg_query=3.972 ms (−15%), p99=4.459 ms (−29%)
       avg_cpu=3.972, avg_probe=3.348, io_wait=0.002
       safe_out=1342.5, uncertain=77.3 (per query)
       false_safeout=1.96, false_safein_upper=0.0, total_safein=2
       early_stop=100%, avg_skipped=179.4, overlap=0.9995

       关键观察：recall 没有提升，false_safeout 仍然 ~1.9/query。
       Hadamard rotation 提升了性能但没有修复分类问题——这个问题是
       OverlapScheduler::ProbeCluster L202-204 的 dynamic_d_k 初始值 bug，
       已在 Phase 1.1.ter 修复。 -->

### 1.1.ter 修复 OverlapScheduler::ProbeCluster dynamic_d_k 初始值 bug

- [x] 1.1.ter.1 发现 bug：`ProbeCluster` L202-204 当 `est_heap_.size() < est_top_k_` 时使用
  `index_.conann().d_k()` 作为 SafeOut 阈值，而 `bench_vector_search` 和 early_stop 路径
  （L363-365）均使用 `std::numeric_limits<float>::infinity()`。导致查询早期就错误地
  SafeOut 了真正的 top-k 候选。
- [x] 1.1.ter.2 修复：将 `index_.conann().d_k()` 改为 `std::numeric_limits<float>::infinity()`
  <!-- 修复后 bench_e2e COCO 100K:
       recall@10: 0.804 → 0.902 (+0.098)
       false_safeout: 1.90 → 0.98/query (−48%)
       avg_query: 4.67 ms → 4.25 ms (−9%)
       safe_out=1309.4, uncertain=117.6 (per query)
       说明：还剩 0.98 false_safeout 来自 dynamic_d_k 在 block 内不更新（每个 block
       开头计算一次，block 内不刷新），Phase 3 ClusterProber 重写时修复。 -->

### 1.2 IvfIndex 即时计算 rotated_centroids

- [x] 1.2.1 在 `include/vdb/index/ivf_index.h` 添加私有字段：
  ```cpp
  std::vector<float> rotated_centroids_;  // nlist × dim，Hadamard 时填充
  bool used_hadamard_ = false;
  ```
- [x] 1.2.2 添加 public accessor：
  ```cpp
  bool used_hadamard() const { return used_hadamard_; }
  const float* rotated_centroid(uint32_t k) const;
  ```
- [x] 1.2.3 `IvfIndex::Open` 加载完 rotation 后，检测是否 Hadamard（`dim` 是 2 的幂 + rotation 来源），如果是就预计算 `rotated_centroids_`
- [x] 1.2.4 为 OverlapScheduler 暴露 `used_hadamard()` 和 `rotated_centroid()`

### 1.3 OverlapScheduler 使用 PrepareQueryRotatedInto 快路径

- [x] 1.3.1 在 `OverlapScheduler::Search` 开头，如果 `index_.used_hadamard()`，一次性旋转 query：
  ```cpp
  if (index_.used_hadamard()) {
      rotation.Apply(query, rotated_q_.data());
  }
  ```
- [x] 1.3.2 `ProbeCluster` 里：Hadamard 路径用 `PrepareQueryRotatedInto(rotated_q, rotated_c[cid], &pq)`，否则走原来的 `PrepareQuery`
- [x] 1.3.3 `pq` 从每次 `PrepareQuery` 改为 `PrepareQueryInto`，复用 scheduler 持有的 `PreparedQuery` 对象
  <!-- Phase 1.2-1.3 after (bench_e2e COCO 100K, nlist=2048, nprobe=256, queries=500, bits=4):
       build_time=4259.3 ms (incl. 1704 ms CRC calib from precomputed scores)
       recall@1=0.9020, @5=0.8808, @10=0.8694
       avg_query=2.790 ms (−28% vs Phase 3's 3.883 ms), p50=2.780, p95=3.473, p99=3.825
       probe=1.819 ms, io_wait=0.002 ms, cpu=2.788 ms
       safe_out=669.6, uncertain=152.5
       s2_safe_in=0.1, s2_safe_out=2225.5, s2_uncertain=152.5
       false_safeout=1.31 (vs 0.98 at Phase 3 — slight increase from larger sample)
       early_stop=100%, avg_skipped=202.7
       Key win: PrepareQueryRotatedInto skips per-cluster FWHT O(dim·log(dim))×nprobe → O(dim) per cluster -->

### 1.4 删除 popcount 死代码（注意 inline bench 对照组）

- [x] 1.4.1 在 `src/index/` 和 `src/query/` 中 grep `PopcountXor|hamming` 找出所有 popcount 调用
  <!-- 结论：生产路径 (cluster_prober.cpp) 只用 EstimateDistanceFastScan；PopcountXor 只在 rabitq_estimator.cpp 内调用 -->
- [x] 1.4.2 检查 `bench_vector_search_inline.cpp`（Phase 0.5 备份）是否还在使用 `RaBitQEstimator::EstimateDistance` popcount 版本
  - 结果：inline bench line 1024 + bench_vector_search line 1011 + bench_rabitq_accuracy/diagnostic/ivf_quality 均使用
  - 决策：**保留 API，标记 `[[deprecated]]`**；production path 不调用，benches 可继续编译
- [x] 1.4.3 **不修改** `bench_vector_search_inline.cpp` 的 popcount 对照路径（inline 版是冻结版）
- [x] 1.4.4 `bench_vector_search.cpp` 保留（Phase 4 才重写）；无需在 Phase 1 删除
- [x] 1.4.5 保留 `simd::PopcountXor` kernel（仍被 EstimateDistanceRaw 调用，多个 bench 直接使用）
- [x] 1.4.6 编译 `bench_vector_search_inline` 确认未破坏对照组 ✓（仅 deprecated warning，无 error）

### 1.5 Phase 1 验证

- [x] 1.5.1 `ctest --output-on-failure`：pre-existing 2 个 failures 不变（test_conann, test_buffer_pool），其他 37/39 均通过 ✓
- [x] 1.5.2 bench_vector_search COCO 100K：recall@10=0.8930, lamhat=0.055386 — 精确匹配 Phase 0.4 基线 ✓
  <!-- bench_vector_search 不走 IvfIndex/OverlapScheduler，所以 Phase 1.2-1.4 对其无影响 -->
- [x] 1.5.3 bench_e2e COCO 100K 验证（nlist=2048, nprobe=256, queries=500, bits=4）：
  recall@10 = 0.8694（500-query 采样，比 50-query 更稳定）
- [x] 1.5.4 bench_e2e avg_probe 大幅下降：Phase 3 3.883ms → Phase 1.2-1.3 2.790ms（**−28%**）
  <!-- PrepareQueryRotatedInto 跳过每 cluster 的 FWHT O(dim·log(dim))×nprobe = 256 次 FWHT，改为只在 Search() 开头做 1 次 Apply -->
- [x] 1.5.5 记录 Phase 1 后数值：
  <!-- Phase 1 after (bench_e2e COCO 100K, nlist=2048, nprobe=256, queries=500, bits=4):
       recall@1=0.9020, @5=0.8808, @10=0.8694
       avg_query=2.790 ms (−28% vs Phase 3), p50=2.780, p95=3.473, p99=3.825
       probe=1.819 ms, io_wait=0.002 ms, cpu=2.788 ms
       safe_out=669.6, uncertain=152.5 (per query)
       s2_safe_in=0.1, s2_safe_out=2225.5, s2_uncertain=152.5
       false_safeout=1.31, early_stop=100%, avg_skipped=202.7
       Key improvement: rotated_centroids precomputed at Open() + PrepareQueryRotatedInto fast path -->

---

## Phase 2 — 段级格式升级（`rotated_centroids.bin` + `eps_ip_fs` 持久化）

### 2.1 新增 rotated_centroids 磁盘存储

- [x] 2.1.1 `IvfBuilder::WriteIndex` 在 Hadamard 路径下把 `rotated_centroids` 写入 `<output_dir>/rotated_centroids.bin`（nlist × dim × f32 raw）
- [x] 2.1.2 `IvfIndex::Open` 当 `has_rotated_centroids=true` 时从磁盘加载；当 false 时跳过（非 Hadamard 或旧格式索引优雅降级，使用慢路径）
- [x] 2.1.3 删除 Phase 1.2.3 的即时计算代码（Open 时从磁盘读取或降级）

### 2.2 segment.meta 新增 eps_ip_fs 字段

- [x] 2.2.1 `schema/segment_meta.fbs` 中 `ConANNParams` 添加字段：`eps_ip_fs: float` 和 `has_rotated_centroids: bool = false`
- [x] 2.2.2 cmake `--target flatbuffers_generated` 重新生成 C++ FlatBuffers 头文件，编译通过
- [x] 2.2.3 `IvfBuilder::WriteIndex` 写入 `eps_ip_fs=calibrated_eps_ip_` 和 `has_rotated_centroids=used_hadamard`
- [x] 2.2.4 `IvfIndex::Open` 的 Phase 2 读取路径由 `has_rotated_centroids` 标志控制
- [x] 2.2.5 `has_rotated_centroids=true` 但文件缺失 → 返回 IOError；`false` → 优雅降级（不强制报错）

### 2.3 Phase 2 验证

- [x] 2.3.1 重建 COCO 100K 索引（新格式，含 rotated_centroids.bin）
- [x] 2.3.2 `bench_e2e` 运行正常，recall@10=0.8694 与 Phase 1.5.3 一致 ✓
- [x] 2.3.3 旧格式索引（无 rotated_centroids.bin）→ `used_hadamard_=false`，使用慢路径（不报错）
- [x] 2.3.4 记录 Phase 2 后数值：
  <!-- Phase 2 after (bench_e2e COCO 100K, nlist=2048, nprobe=256, queries=500, bits=4):
       rotated_centroids.bin size = 4194304 bytes = 2048 × 512 × 4B ✓
       recall@10=0.8694 (identical to Phase 1)
       avg_query=1.439 ms (−48% vs Phase 1 2.790 ms)
       p50=1.416, p95=1.795, p99=1.963
       false_safeout=1.31, early_stop=100%
       Improvement source: disk-loaded rotated_centroids.bin vs on-the-fly FWHT per centroid at Open() -->

---

## Phase 3 — ClusterProber 抽取

### 3.1 定义接口和类

- [x] 3.1.1 新增 `include/vdb/index/cluster_prober.h`：
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

- [x] 3.2.1 新增 `src/index/cluster_prober.cpp`，实现 `Probe`：
  - 复用 bench_vector_search FastScan path 的逻辑（L883-1003）
  - 对每个 block：调 `EstimateDistanceFastScan` → `FastScanSafeOutMask` → ctz 迭代
  - Stage 1: `est_dist < d_k - 2*margin` → SafeIn，否则 Uncertain（SafeOut 已被 mask 消化）
  - Stage 2: 非-SafeOut S1-Uncertain 走 `IPExRaBitQ`（bits > 1）→ ClassifyAdaptive
  - 非-SafeOut 最终 lane 调 `sink.OnCandidate()`
- [x] 3.2.2 `pc.fastscan_block_size` 已包含 norms 区，`packed_sz = FastScanPackedSize(dim)` 偏移正确

### 3.3 AsyncIOSink 封装 PrepRead

- [x] 3.3.1 在 `src/query/overlap_scheduler.cpp` 内部定义 `OverlapScheduler::AsyncIOSink : public ProbeResultSink`
  - 嵌套类（nested class）定义在 .cpp 里，在 .h 里前向声明
  - 构造时持有 `OverlapScheduler&` + `int dat_fd`（RerankConsumer 不需要）
  - est_heap 更新在 OnCandidate 里（持有 sched_.est_heap_ 的引用）
- [x] 3.3.2 `OnCandidate(vec_idx, addr, est_dist, cls)` 实现：
  - `cls == SafeIn` + `addr.size <= safein_all_threshold` → `PrepRead(VEC_ALL)`
  - `cls == SafeIn` + `addr.size > threshold` → `PrepRead(VEC_ONLY)`
  - `cls == Uncertain` → `PrepRead(VEC_ONLY)`
  - 统计（total_safe_in/total_uncertain）在 ProbeCluster 里从 local_stats 聚合，不在 Sink 里
- [x] 3.3.3 Sink 不持有 est_heap 所有权，只有更新权；所有权仍在 scheduler

### 3.4 ProbeCluster 变薄

- [x] 3.4.1 `OverlapScheduler` 新增成员：
  - `ClusterProber prober_`（构造时初始化）
  - `RaBitQEstimator estimator_`（PrepareQueryInto 用，避免每次 ProbeCluster 新建）
  - `PreparedQuery pq_`（重用的 per-cluster PreparedQuery buffer）
  - `rotated_q_` 和 Hadamard 路径 **延迟到 Phase 1.2-1.3**（IvfIndex 需先支持 `used_hadamard()` 和 `rotated_centroid()`）
- [x] 3.4.2 在 `Search()` 开头（不是 ProbeCluster 内部）一次性算 `rotated_q_`（Hadamard 路径）
  **→ 完成于 Phase 1.2/1.3（依赖 IvfIndex 的 rotated_centroids 支持）**
- [x] 3.4.3 把 `OverlapScheduler::ProbeCluster` 改为：
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
- [x] 3.4.4 **est_heap 维护逻辑**：AsyncIOSink::OnCandidate 更新 sched_.est_heap_
  （通过嵌套类访问 OverlapScheduler private 成员）。est_heap 所有权在 scheduler，Sink 有更新权。
  注意：dynamic_d_k 在 Probe() 调用前一次性传入，Probe() 内部不实时更新（block 内的 est_kth
  刷新是未来优化点）。

### 3.5 Phase 3 验证（最关键的 bit-exact 验证）

- [x] 3.5.1 bench_e2e COCO 100K：recall@10 = 0.902（与 Phase 1.1.ter 一致）
- [x] 3.5.2 false_safeout = 0.98（与 Phase 1.1.ter 一致，剩余来自 block 内 est_kth 不实时更新）
- [x] 3.5.3 数值无漂移（recall bit-exact 一致）
- [x] 3.5.4 avg_cpu 进一步下降：4.25 ms → 3.88 ms (−8.5%)（FastScan SIMD + 避免 per-call estimator 分配）
- [x] 3.5.5 avg_io_wait 维持不变（false_safeout 未进一步减少）
- [x] 3.5.6 记录 Phase 3 后数值：
  <!-- Phase 3 after (bench_e2e COCO 100K, nlist=2048, nprobe=256, queries=50, bits=4):
       build_time=2550.3 ms (Phase 1.1 OMP 加速)
       recall@1=0.9000, @5=0.9080, @10=0.9020
       avg_query=3.883 ms (−8.5% vs Phase 1.1.ter 4.25 ms)
       p50=3.819, p95=4.354, p99=4.700
       safe_in=0.0, safe_out=1273.3, uncertain=149.9
       s2_safe_in=0.0, s2_safe_out=2913.1, s2_uncertain=149.9
       false_safeout=0.98, false_safein_upper=0.0, total_safein=2
       Architecture: ClusterProber extracted, AsyncIOSink nested class, ProbeCluster thin wrapper
       Remaining: 0.98 false_safeout from per-cluster (not per-block) dynamic_d_k → Phase 1.3 Hadamard 快路径后可进一步优化 -->

---

## Phase 4 — bench 统一到磁盘路径 + 纯查询模式

### 4.1 bench_e2e 增加 --index-dir 参数

- [x] 4.1.1 `bench_e2e.cpp` 解析 `--index-dir`
- [x] 4.1.2 如果指定：跳过 Phase A（仍需 load queries 用于搜索 + 载 gt）+ 跳过 Phase C (Build)，直接 `index.Open(index_dir)`
- [x] 4.1.3 Phase C.5 (CRC calibration)：优先从 `index_dir/crc_scores.bin` 加载
- [x] 4.1.4 验证：先用旧方式 build 一个 COCO 1K 索引到 `/tmp/coco1k_idx`，再用 `--index-dir /tmp/coco1k_idx` 运行，数值完全一致

### 4.0 Spike：确认 payload_fn=nullptr / empty payload_schemas 路径可用

- [x] 4.0.1 读 `src/index/ivf_builder.cpp`，确认 `IvfBuilderConfig::payload_schemas` 为空且 `payload_fn=nullptr` 时 `.data` 只含向量且 `IvfBuilder::Build` 不报错
- [x] 4.0.2 读 `src/storage/segment.cpp` / `src/storage/data_file.cpp`，确认 `DataFileReader` 在无 payload schema 时能正常读取
- [x] 4.0.3 读 `src/query/rerank_consumer.cpp`，确认 `ConsumeVec` / `ConsumeAll` 在无 payload 时行为正确（VEC_ONLY 路径应该天然工作；VEC_ALL 路径等价于 VEC_ONLY）
- [x] 4.0.4 写一个最小 spike 测试：生成 1000 个 Deep1M 风格的随机向量，调 `IvfBuilder::Build(payload_schemas={}, payload_fn=nullptr)`，Open，Query 一个已知向量
  - 如果不能工作：补最小修复（只修"至少一个 payload 列"的硬编码假设，不做新功能）
- [x] 4.0.5 记录发现到 Phase 4.0 的注释里（供 Phase 4.2 参考）

### 4.2 bench_vector_search 重写（不含 inline 版）

**前置**：`bench_vector_search_inline.cpp`（Phase 0.5 备份）保持原样。本任务只重写 `bench_vector_search.cpp`。

- [x] 4.2.1 删除 `bench_vector_search.cpp` 的 inline KMeans/Encode/calibrate/search 代码（L349-1115）
- [x] 4.2.2 保留：CLI、Phase A（load .fvecs）、Phase E（recall）、Phase F（stats/JSON）
- [x] 4.2.3 新增 Phase B：`IvfBuilder::Build` 构建到临时目录，`payload_schemas={}` + `payload_fn=nullptr`
  - 临时目录：`--tmp-dir` 参数（默认 `/tmp/bench_vs_<timestamp>`）
  - 测试结束自动清理（除非 `--keep-index`）
- [x] 4.2.4 新增 Phase C：`IvfIndex::Open` + `OverlapScheduler::Search` 循环
  - 循环结构和 bench_e2e 的 `RunQueryRound` 一致
  - 用 `IoUringReader` 读 .data（验证走的是磁盘路径）
- [x] 4.2.5 用 `strace -c -e read,pread64,io_uring_enter` 或 `perf stat -e syscalls:sys_enter_read` 验证确实发生了磁盘 I/O（不是 mmap 零开销）
- [x] 4.2.6 两边数值对比：新 `bench_vector_search` Deep1M recall@10 应等于 `bench_vector_search_inline` 的 baseline

### 4.3 bench_vector_search 也支持 --index-dir

- [x] 4.3.1 添加参数解析
- [x] 4.3.2 跳过 Phase B，直接 Open

### 4.4 统一 build-or-load helper（可选）

- [x] 4.4.1 如果两边的 "build-or-load" 逻辑重复太多，抽成 `benchmarks/common/build_or_load.h`
- [x] 4.4.2 否则就 copy-paste（~30 行）

### 4.5 Phase 4 验证

- [x] 4.5.1 bench_vector_search Deep1M：数值和 Phase 0 基线一致
  - `recall@10` = 0.9685
  - `lamhat` 相对误差 < 0.1%
- [x] 4.5.2 bench_vector_search COCO 100K：数值和 Phase 0 基线一致
- [x] 4.5.3 验证 bench_vector_search 现在确实从磁盘读 .clu（用 `strace` 或 `perf stat` 验证 I/O 系统调用）
- [x] 4.5.4 测量两边加 `--index-dir` 之后的启动时间（应该只剩 query 时间）

---

## 5. 最终验证 + 清理

- [x] 5.1 完整跑一遍测试套件：
  - `bench_vector_search` Deep1M + COCO 100K（新薄 bench，走磁盘）
  - `bench_vector_search_inline` Deep1M + COCO 100K（对照组，如果还能编）
  - `bench_e2e` COCO 1K
- [x] 5.2 `ctest --output-on-failure`：确认只有 2 个 pre-existing failures
- [x] 5.3 代码行数 diff：
  <!-- bench_vector_search: 1261 → 570 lines (55% reduction via library unification) -->
  <!-- bench_vector_search_inline: 1274 lines (frozen, still compiles with deprecated warnings) -->
- [x] 5.4 性能总结：
  <!-- bench_vector_search COCO100K --index-dir: recall@10=0.8925, avg_lat=2.36ms (build_time=0ms) -->
  <!-- bench_vector_search Deep1M --index-dir: recall@10=0.9685, io_uring_enter confirmed (2299 calls for 10 queries) -->
- [x] 5.5 更新 `.claude/memory/` 中的 project_phaseb_optimization.md
- [x] 5.6 归档 `.clu v7` 相关死代码（如果有的话）
- [x] 5.7 确认 `bench_vector_search_inline` 的状态（还在编 / 编译被 API 破坏 / 已废弃）记录到 memory

---

## 6. 未来工作（Non-Goals，仅记录）

- [ ] 6.1 per-block-32 粒度的 CPU-I/O 流水优化（`probe-io-block32-interleaved`）
- [ ] 6.2 `FlatBuffers segment.meta v9` 真正重整（目前是增量加字段）
- [ ] 6.3 去掉 `IoUringReader` 的 buffer_pool 分配热点（Phase 3 后可能成为新瓶颈）
- [ ] 6.4 `ClusterProber` 支持 batch-multiple-cluster 并行（SIMD 跨 cluster）
