# Remaining Optimization List

当前基线以 `eps=0.90`、`v11 compact_blocked`、`nprobe=64`、`full_preload + resident=1`、`crc=1` 为准。

参考结果：
- `avg_query ≈ 0.495 ~ 0.502 ms`
- `coarse_select ≈ 0.060 ms`
- `probe_prepare ≈ 0.066 ~ 0.067 ms`
- `probe_stage2 ≈ 0.189 ~ 0.196 ms`
- `probe_submit ≈ 0.022 ms`
- `uring_submit ≈ 0.082 ~ 0.084 ms`
- `safe_out ≈ 1882`
- `uncertain ≈ 91.8`
- `unique_fetch ≈ 91.9`

`query-only + perf stat` 的硬件计数器画像：
- `IPC = 1.33`
- `frontend idle = 23.63%`
- `branch-miss = 6.09%`
- `cache-miss = 7.82%`
- `L1-dcache-load-miss = 10.79%`

结论：
- 当前还没有到硬件上限。
- I/O wait 已基本隐藏，主剩余空间在在线 CPU 路径。
- 当前更像是“前端效率 + 控制流 + cache 局部性 + 少量固定软件骨架成本”的综合瓶颈。

---

## P0: 前端与控制流优化

这类优化优先级最高，因为 `frontend idle` 偏高，`branch-miss` 也偏高，说明在线路径还没有收敛成足够平滑的 hot path。

### 1. 收敛 `ProbeCluster()` 主热路径

目标：
- 让最常见路径只走一条稳定分支
- 把少见路径和兼容路径彻底挪到冷分支

重点位置：
- `src/index/cluster_prober.cpp`
- `src/query/overlap_scheduler.cpp`

具体建议：
- 把 `v11 compact_blocked + packed_sign + resident` 视为正式主路径，单独拉成最薄的 fast path。
- 把 `v10`、非 packed-sign、非 resident、尾块兼容逻辑尽量下沉到冷路径 helper。
- 将 `Stage2` 中 lane-mask 的稀疏分支组织进一步线性化，减少 per-lane 条件跳转。

预期收益：
- `0.02 ~ 0.05 ms`

### 2. 继续压 query prepare 的控制流碎片

目标：
- 压低 `PrepareQueryRotatedInto`、`QuantizeQuery14BitWithMaxToFastScanLUT` 的前端开销

重点位置：
- `src/rabitq/rabitq_estimator.cpp`
- `src/simd/fastscan.cpp`

具体建议：
- 把 prepare 中仍然存在的小 helper 和尾路径内联/合并到稳定块中。
- 减少 prepare 阶段的多段函数调用边界。
- 检查 `quant_query / LUT` 生成时是否仍有冷路径分支混在热循环里。

预期收益：
- `0.01 ~ 0.03 ms`

---

## P0: Stage2 与数据局部性优化

`probe_stage2` 仍是最大的在线块。`compact_blocked` 已经起效，但还没有把 locality 吃满。

### 3. 继续压 `IPExRaBitQBatchPackedSignCompact`

目标：
- 让 Stage2 更接近连续流式消费，减少 cache 层级切换

重点位置：
- `src/simd/ip_exrabitq.cpp`
- `include/vdb/simd/ip_exrabitq.h`

具体建议：
- 继续检查 `batch=8` block 内部的访问顺序，优先保证：
  - query block 一次加载
  - `abs/sign/xipnorm` 尽量线性消费
- 继续压 block 内尾部处理，把 tail path 从主循环里拿掉。
- 检查 `64/32/16/tail` 过渡处是否仍有多余的临时数据重组。
- 如果后续考虑进一步微结构优化，可试：
  - 更激进的 unroll
  - 更少的中间寄存器状态
  - 减少 sign 解包中的重复位操作

预期收益：
- `0.02 ~ 0.04 ms`

### 4. 针对 compact block 再做一次布局细抠

目标：
- 提升 `v11` block 内部的 cache line 利用率

重点位置：
- `src/storage/cluster_store.cpp`
- `include/vdb/query/parsed_cluster.h`

具体建议：
- 复查当前 `abs_plane / sign_plane / xipnorm[8]` 的实际线性顺序是否完全匹配 kernel 的消费顺序。
- 若 kernel 先吃 `abs` 再吃 `sign`，则保证 block 内顺序与 kernel 顺序一致。
- 评估是否值得让 `xipnorm[8]` 和块头 metadata 更靠近，减少 block 尾部跳转。
- 复查尾块 padding 的写法，避免尾块路径在 query 期触发额外条件处理。

预期收益：
- `0.01 ~ 0.03 ms`

---

## P1: Coarse / Prepare 固定成本

### 5. 继续压 coarse select

当前：
- `coarse_select ≈ 0.060 ms`

重点位置：
- `src/index/ivf_index.cpp`

具体建议：
- 已有 packed centroid score SIMD 后，继续查：
  - score 写回是否过散
  - top-n 前的 scratch/order 是否还存在多余搬运
- 如果 coarse 进一步成为相对大项，可再看 top-n selection 的重写价值。

预期收益：
- `0.01 ~ 0.02 ms`

### 6. 继续压 `probe_prepare`

当前：
- `probe_prepare ≈ 0.066 ~ 0.067 ms`

重点位置：
- `src/rabitq/rabitq_estimator.cpp`

具体建议：
- 继续减少 prepare 阶段的 scratch 读写与清零。
- 检查 `rotated / quant_query / fastscan_lut` 是否仍有可进一步复用的空间。
- 默认 benchmark 口径下，继续保持低开销计时，不再回到细粒度打点。

预期收益：
- `0.01 ~ 0.02 ms`

---

## P1: Submit / I/O 软件骨架

I/O wait 现在基本为 0，但 `uring_submit` 仍有固定软件成本。

### 7. 继续压 `submit_calls` 与 `uring_submit`

当前：
- `submit_calls ≈ 5.9`
- `uring_submit ≈ 0.082 ~ 0.084 ms`

重点位置：
- `src/query/overlap_scheduler.cpp`

具体建议：
- 继续减少尾部 flush。
- 保持当前 scheduler 级 pending queue 设计，进一步减少小批次提交。
- 继续把 `submit_prepare_vec_only / submit_emit` 做线性化，避免小批次抖动。

预期收益：
- `0.01 ~ 0.02 ms`

### 8. 检查 `remaining_payload_fetch`

当前已经很小：
- `remaining_payload_fetch ≈ 0.010 ~ 0.011 ms`

这条线不建议优先投入，除非后面要冲更极限的低时延。

---

## AOCL 可选路径

用户环境里有：
- `~/opt/AMD/aocl`

当前还没有证据表明必须引入 AOCL 才能继续优化，但它可以作为后续探索项。

### 9. AOCL/BLAS 用于 coarse score 或某些批量向量核

适用位置：
- coarse centroid score
- 某些可批量化的 dot/GEMV 路径

判断原则：
- 只有在现有自研 SIMD kernel 已经明显接近上限、且 perf 显示计算核稳定主导时，再考虑接入。
- 目前从 `query-only + perf stat` 看，还没到这一步。

风险：
- 接入成本高
- 小批量场景不一定比现有手写 kernel 更好
- 很可能引入额外数据搬运

当前建议：
- 暂不优先接入
- 可作为后续“coarse/批量 dot kernel”的对照实验项

---

## 不建议当前优先做的方向

### 10. 不建议继续优先调 I/O 等待

原因：
- `io_wait = 0`
- `prefetch_wait = 0`
- `overlap ≈ 1.0`

说明磁盘等待基本已经不是主问题。

### 11. 不建议回到 `eps` 大范围搜索

原因：
- 已经验证 `eps=0.90` 显著优于当前 `0.99`
- 当前瓶颈分析目标应转回在线路径，而不是继续扫描参数

### 12. 不建议优先做更激进的 rebuild / 新格式

原因：
- `v11 compact_blocked` 当前已经是正确且有效的
- 现在的主要收益更可能来自 query 主路径优化，而不是再次改格式

---

## 推荐执行顺序

1. `Stage2 compact kernel` 与 `ProbeCluster` 热路径收敛
2. `PrepareQuery / LUT` 固定成本压缩
3. `coarse_select` 继续压缩
4. `submit_calls / uring_submit` 收尾
5. AOCL 仅作为后续对照探索项

---

## 当前判断

基于 `query-only + perf stat`：
- `IPC = 1.33`
- `frontend idle = 23.63%`
- `branch-miss = 6.09%`
- `cache-miss = 7.82%`
- `L1 miss = 10.79%`

当前更像：
- 前端效率问题
- 控制流问题
- cache/locality 问题

而不是：
- 纯算力上限
- 纯内存带宽上限
- 纯 I/O 硬件上限

因此，当前仍有明确的软件优化空间。
