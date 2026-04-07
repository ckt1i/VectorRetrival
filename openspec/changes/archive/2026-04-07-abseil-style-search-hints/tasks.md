# 任务清单：Abseil 风格的向量搜索优化

每个任务都可独立测试。按顺序执行，每做完一个就 benchmark，确认 recall 没有回归才继续下一个。

## Stage A: 低风险快速收益（最低工作量，最快验证）

### Task A1: 用 `std::partial_sort` 替换 centroid 排序的 `std::sort`

- [x] Done — 实测 2.0ms → 1.66ms (-0.34ms, 远好于预期的 -0.10ms)

### Task A2: 消除 `PrepareQueryInto` 中冗余的 memset

- [x] Done — 实测 1.663ms → 1.666ms (噪声范围内, 节省太小不可见). 正确性: debug 用 0xCD 哨兵验证 BuildFastScanLUT 覆盖全部字节。

### Task A3: 把 `est_kth` 提到内层循环外

- [x] Done — 实测 1.666ms → 1.535ms (-0.13ms, 好于预期 -0.05ms). 在 FastScan + popcount 两条路径上都改了, 修改 heap 后刷新缓存值。

## Stage B: 内存布局（中等工作量）

### Task B1: 把 `soa_norms` 和 `soa_xipnorm` 合并成 `NormPair` 数组

- [x] Done — 实测 1.535ms → 1.543ms (噪声内). 在 Phase 2b 已加 `_mm_prefetch(&soa_norms[vid])` 之后, 两次 cache line miss 已基本消除, 进一步合并收益不可见。语义和正确性都验证通过。

## Stage C: SIMD 特化（中等工作量，需要仔细测试）

### Task C1: 向量化 FastScan 距离反量化

- [x] Done — 实测 1.543ms → 1.560ms (噪声内). SIMD 实现放在 `src/simd/fastscan.cpp:FastScanDequantize`, 由 `EstimateDistanceFastScan` 调用. 编译器对原标量循环已经做了较好的自动向量化, 显式 AVX-512 改写没有显著收益, 但代码组织更清晰且锁定了路径。

### Task C2: 向量化 SafeOut 分类

- [x] Done — 实测 1.560ms → 1.452ms (-0.11ms, 符合预期). SIMD 实现放在 `src/simd/fastscan.cpp:FastScanSafeOutMask`, benchmark 用 `__builtin_popcount`/`__builtin_ctz` 迭代非 SafeOut lane. recall@10 + 最终分类计数完全一致; 中间 `s1_safeout` 比 scalar 少 1414 (相应 `s1_uncertain` 多 1414), 因为批量化 mask 使用 block 开头的 `est_kth`, 部分 lane 跳过了中途更新, 最终被 Stage 2 重新剪枝, 不影响最终结果。

## Stage D: 最终 benchmark 与 profile

### Task D1: 汇总 benchmark + profile

- [x] Done

**5-run results (MKL_NUM_THREADS=1 OMP_NUM_THREADS=1, nprobe=100)**

| 配置 | latency avg (5 runs) | 中位数 | recall@10 |
|------|---------------------|--------|-----------|
| `--crc 1` | 1.456 / 1.431 / 1.442 / 1.411 / 1.406 | **1.431 ms** | 0.9650 |
| `--crc 0` | 1.465 / 1.471 / 1.470 / 1.517 / 1.462 | **1.470 ms** | 0.9900 |

**目标达成**: latency avg ≤ 1.5ms ✓, recall@10 ≥ 0.95 ✓

**完整优化历程 (deep1m, nprobe=100, bits=4)**

| 阶段 | latency avg | recall@10 (CRC) | 累计加速 |
|------|-------------|-----------------|----------|
| Original (baseline) | 5.107 ms | 0.9900 | 1.0× |
| Phase 1 (MKL+flags) | 2.577 ms | 0.9650 | 2.0× |
| Phase 2 (SOA+prefetch fix) | 2.251 ms | 0.9650 | 2.3× |
| Phase 2b (gt_set fix) | 1.989 ms | 0.9650 | 2.6× |
| **Phase 3 (Abseil hints)** | **1.431 ms** | **0.9650** | **3.6×** |
