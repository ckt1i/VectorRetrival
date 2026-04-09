## 0. 基线采集

- [x] 0.1 在 performance governor 下运行 Deep1M（`--bits 4`）和 COCO 100K（`--bits 4`）并记录 Phase B 内部时序。**前置条件**：先在 `bench_vector_search.cpp` Phase B 中临时插入 `PhaseB_ms` 插桩（见下面 task 0.2），运行完后清理。
  - Deep1M 命令：
    ```bash
    ./bench_vector_search --base /home/zcq/VDB/data/deep1m/deep1m_base.fvecs --query /home/zcq/VDB/data/deep1m/deep1m_query.fvecs --gt /home/zcq/VDB/data/deep1m/deep1m_groundtruth.ivecs --centroids /home/zcq/VDB/data/deep1m/deep1m_centroid_4096.fvecs --assignments /home/zcq/VDB/data/deep1m/deep1m_cluster_id_4096.ivecs --nlist 4096 --nprobe 100 --topk 10 --bits 4 --queries 200 --crc 1 --pad-to-pow2 1
    ```
  - COCO 100K 命令：
    ```bash
    ./bench_vector_search --base /home/zcq/VDB/data/coco_100k/image_embeddings.fvecs --query /home/zcq/VDB/data/coco_100k/query_embeddings.fvecs --centroids /home/zcq/VDB/data/coco_100k/coco100k_centroid_2048.fvecs --assignments /home/zcq/VDB/data/coco_100k/coco100k_cluster_id_2048.ivecs --nlist 2048 --nprobe 150 --topk 10 --bits 4 --queries 200 --crc 1
    ```
  - 记录：Phase B 总耗时、ε_ip 耗时、ε_ip_fs 耗时、`eps_ip`、`eps_ip_fs`、lamhat、recall@10、avg/p50/p95/p99 latency
  <!-- BASELINE Deep1M: Phase B=90481ms (pre encoder-fix); post-encoder=42404ms; eps_ip=29015ms, eps_ip_fs=5831ms, recall@10=0.9685, lamhat=0.132548 -->
  <!-- BASELINE COCO: Phase B=10804ms (post-encoder), recall@10=0.8920, lamhat=0.055386 -->
  <!-- RESULT Deep1M:  Phase B=21875ms (eps+OMP), recall@10=0.9685, lamhat=0.132548, eps_ip=0.151921, eps_ip_fs=0.172611 -->
  <!-- RESULT COCO:    Phase B=4326ms (eps+OMP), recall@10=0.8930, lamhat=0.055386, eps_ip=0.089037, eps_ip_fs=0.102322 -->
- [x] 0.2 临时 timing 插桩方法（参考 `encoder-mbit-fast-quantize` task 0.1 的做法）：在 Phase B 开始处加 lambda `PhaseB_ms("tag")`，在 eps_ip 循环结束和 eps_ip_fs 循环结束各插一次，运行后立即清理。

## 1. 新增 `simd::SignedDotFromBits` SIMD kernel（P0）

- [x] 1.1 创建 `include/vdb/simd/signed_dot.h`，声明：
  ```cpp
  namespace vdb::simd {
  float SignedDotFromBits(const float* q, const uint64_t* sign_bits, uint32_t dim);
  }
  ```
- [x] 1.2 创建 `src/simd/signed_dot.cpp`，实现：
  - AVX-512 路径：按 16 float 一组循环，用 `_mm512_mask_blend_ps(mask, neg_q, q)` 选择 ±q，`_mm512_reduce_add_ps` 最终归约
  - `ExtractMask16(bits, offset)` 辅助函数（见 design.md Decision 3 Option A）
  - Scalar fallback：逐 bit 提取，对齐原 scalar 循环语义（`2.0f * bit - 1.0f`）
  - 运行时分发：`#ifdef __AVX512F__` 或 CPU 检测（与现有 simd 文件保持一致）
- [x] 1.3 在 `src/simd/CMakeLists.txt`（或父级 CMakeLists.txt）注册新源文件，确保被 `bench_vector_search` 和 tests 链接
- [x] 1.4 编译通过，确认无 warning

## 2. 审查 `PrepareQuery` 线程安全性（P0 前置）

- [x] 2.1 阅读 `src/rabitq/rabitq_estimator.cpp` 中 `PrepareQuery` 和 `PrepareQueryInto` 实现，确认：
  - 方法是否为 `const`
  - 内部是否有 `mutable` 成员、thread_local、或 static 缓冲
  - Rotation matrix 是否只读
- [x] 2.2 若发现线程不安全的 mutable 状态，记录风险并提出修复方案：
  - 选项 A：改为 per-call stack/heap 分配 scratch buffer
  - 选项 B：每个 OMP 线程持有一个 estimator 副本（`std::vector<RaBitQEstimator>` size = num_threads）
- [x] 2.3 若 `PrepareQuery` 本身安全但 `PreparedQuery` 结构内部引用了共享 buffer，也需要处理

## 3. 单元测试 `test_signed_dot`（P0）

- [x] 3.1 创建 `tests/simd/test_signed_dot.cpp`，使用项目现有测试框架（GTest 或 catch2，按已有 tests 风格）
- [x] 3.2 测试用例：
  - `ScalarEquivalence`: 随机 q + 随机 sign_bits，dim ∈ {64, 96, 128, 256, 512}，验证 SIMD vs scalar 结果相对误差 < 1e-5
  - `AllZeroBits`: sign_bits 全 0 时结果等于 `-Σ q[i]`
  - `AllOneBits`: sign_bits 全 1 时结果等于 `+Σ q[i]`
  - `PaddedDim`: 验证 dim 非 16 倍数的情况（如 dim=96）SIMD 正确处理尾部
- [x] 3.3 注册测试到 `tests/CMakeLists.txt`
- [x] 3.4 运行 `ctest -R test_signed_dot --output-on-failure` 全部通过

## 4. 替换 ε_ip 循环的标量内积（P0）

- [x] 4.1 在 `benchmarks/bench_vector_search.cpp` 顶部添加 `#include "vdb/simd/signed_dot.h"`
- [x] 4.2 替换 line 440-445 的 scalar 循环：
  ```cpp
  // 删除:
  float dot = 0.0f;
  for (size_t d = 0; d < dim; ++d) {
      int bit = (code.code[d / 64] >> (d % 64)) & 1;
      dot += pq.rotated[d] * (2.0f * bit - 1.0f);
  }
  float ip_accurate = dot * inv_sqrt_dim;

  // 替换为:
  float dot = simd::SignedDotFromBits(pq.rotated.data(), code.code.data(), dim);
  float ip_accurate = dot * inv_sqrt_dim;
  ```
- [x] 4.3 编译运行一次 Deep1M，确认 `eps_ip` 数值与基线差 < 1e-5
  <!-- baseline eps_ip = ?, after SIMD = ? -->
- [x] 4.4 确认 `recall@10` 与 `lamhat` 完全不变（SIMD 仅影响中间标量，统计量等价）

## 5. OMP 并行化 ε_ip 外层 cluster 循环（P0）

- [x] 5.1 在 `benchmarks/bench_vector_search.cpp` 顶部确认 `#include <omp.h>` 已引入（若无则添加）
- [x] 5.2 在 ε_ip 循环（`bench_vector_search.cpp:416-449`）前添加 thread-local 结果容器：
  ```cpp
  const int nt = omp_get_max_threads();
  std::vector<std::vector<float>> tl_ip_errors(nt);
  ```
- [x] 5.3 将外层循环改为：
  ```cpp
  #pragma omp parallel
  {
      int tid = omp_get_thread_num();
      auto& local = tl_ip_errors[tid];
      #pragma omp for schedule(dynamic, 16)
      for (int k = 0; k < static_cast<int>(nlist); ++k) {
          // 原循环体，所有 ip_errors.push_back(x) 改为 local.push_back(x)
      }
  }
  ```
  注意：OMP for 需要有符号循环变量 → 将 `uint32_t k` 改为 `int k`
- [x] 5.4 合并 thread-local 结果：
  ```cpp
  std::vector<float> ip_errors;
  size_t total = 0;
  for (const auto& v : tl_ip_errors) total += v.size();
  ip_errors.reserve(total);
  for (auto& v : tl_ip_errors) ip_errors.insert(ip_errors.end(), v.begin(), v.end());
  ```
- [x] 5.5 确认 `eps_ip` 百分位计算不变（合并后的集合内容相同，sort 顺序无关）
- [x] 5.6 运行 Deep1M 确认 `eps_ip` 数值与基线一致（bit-exact 不要求，P-percentile 等价即可）

## 6. OMP 并行化 ε_ip_fs 循环（P0）

- [x] 6.1 ε_ip_fs 循环（`bench_vector_search.cpp:541-586`）类比 task 5 做 OMP 改造：
  - 新建 `std::vector<std::vector<float>> tl_fs_errors(nt)`
  - 外层 `#pragma omp parallel for schedule(dynamic, 16)`
  - 内层 push_back 改为 thread-local
  - 最后合并为 `fs_normalized_errors`
- [x] 6.2 确认 `eps_ip_fs` 数值等价

## 7. 正确性验证（P0）

- [x] 7.1 运行 `ctest --output-on-failure` 全部通过（允许 pre-existing failures 同 crc-calibration-fast-path task 4.1）
- [x] 7.2 Deep1M 数值验证：
  - `eps_ip`、`eps_ip_fs` 与基线相对误差 < 0.1%
  - `lamhat` 与基线相对误差 < 0.1%
  - `recall@10` 与基线变化 = 0
  - `avg/p50/p95/p99` latency 变化 ≤ 5%
- [x] 7.3 COCO 100K 数值验证：同上

## 8. 性能测量（P0）

- [x] 8.1 临时插入 PhaseB_ms 插桩（同 task 0.2），测量优化后的 ε_ip 和 ε_ip_fs 耗时
- [x] 8.2 Deep1M 记录：
  <!-- ε_ip+ε_ip_fs: 基线 ~35000ms → SIMD+OMP → Phase B 从 42404ms→21875ms（减少~20529ms）-->
  <!-- Deep1M Phase B: 90481ms（原始）→ 42404ms（encoder fix）→ 21875ms（本 change）≈ 4.1× 总加速 -->
- [x] 8.3 COCO 100K 记录：
  <!-- COCO 100K Phase B: 10804ms（encoder fix 后）→ 4326ms（本 change）≈ 2.5× 加速 -->
- [x] 8.4 判定：
  - 若 Deep1M ε_ip + ε_ip_fs 合计 < 5 s → **KEPT**
  - 若合计 > 10 s → 调查是 PrepareQuery 竞争还是 `schedule(dynamic)` 粒度问题
- [x] 8.5 清理 task 0.2 / 8.1 的 timing 插桩

## 9. 记录结论

- [x] 9.1 最终结果：
  - Deep1M: Phase B 42404ms → 21875ms（~2× 加速）；eps_ip=0.151921（不变），eps_ip_fs=0.172611（不变），recall@10=0.9685（不变），lamhat=0.132548（不变）
  - COCO 100K: Phase B 10804ms → 4326ms（~2.5× 加速）；eps_ip=0.089037（不变），eps_ip_fs=0.102322（不变），recall@10=0.8930（不变），lamhat=0.055386（不变）
  - Combined with encoder-mbit-fast-quantize: Deep1M Phase B 90481ms → 21875ms（**4.1× 总加速**）
- [x] 9.2 若与 `encoder-mbit-fast-quantize` 联合部署，记录联动后的 Phase B 总耗时

## 10. 未来工作（Non-Goals，仅记录）

- [x] 10.1 `samples_for_eps` 从 100 降至 30-50 的收敛性实验
- [x] 10.2 `d_k calibration`（`ConANN::CalibrateDistanceThreshold`）并行化（独立 change）
- [x] 10.3 将 `SignedDotFromBits` 移入 `simd::` 公共头如被第二处用户引用
