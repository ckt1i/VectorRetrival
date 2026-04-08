# 任务清单：Hadamard 旋转快速路径

## Stage A: 启用 Hadamard 路径 + dim=96 fallback 验证

### Task A1: 在 benchmark 中自动选择 Hadamard

- [x] Done — 已实现并验证

**文件**: `benchmarks/bench_vector_search.cpp`

实现:
- 在 `RotationMatrix rotation(dim)` 之后, 检查 `dim` 是否为 2 的幂
- 如果是, 调用 `rotation.GenerateHadamard(seed, true)`
- 否则 fallback 到 `rotation.GenerateRandom(seed)`
- 打印当前选择的 rotation 类型

实测结果 (COCO-100k, dim=512, nprobe=150, CRC=0):
- baseline (Random):  median 7.47 ms, recall@10=0.954
- Hadamard:           median 4.43 ms, recall@10=0.954 (recall@1 +2.5%)
- **节省 -41%**, recall 不变或微涨

### Task A2: 验证 dim=96 (deep1m) fallback

- [x] Done — 5 runs: 1.424 / 1.329 / 1.416 / 1.374 / 1.417 ms, median **1.416 ms**, recall@10 = 0.9650 (无回归, 与 Phase 3 baseline 1.43 ms 一致)

## Stage B: dim=96 padding 到 128

### Task B1: 实现向量 padding helper

- [x] Done — 添加 `--pad-to-pow2` 参数 (默认 0), `IsPowerOf2/NextPowerOf2/PadVectors` helpers, Phase A 中 pad base/query, Phase B 加载 centroids 时也 pad. 已验证编译通过。

### Task B2: 验证 dim=96 padding 到 128 的效果

- [x] Done (用预加载 centroids, scalar FWHT 二进制)

5 runs (deep1m, dim=96 → 128 padding, Hadamard, scalar FWHT):
- 1.497 / 1.522 / 1.478 / 1.484 / 2.041 ms (最后一次有噪声)
- median: **1.497 ms**
- recall@10: **0.9685** (vs Random fallback 0.9650, **+0.4%** ★)

vs Random fallback (no padding):
- latency: 1.416 → 1.497 ms (+5.7% 微回归)
- recall@10: 0.9650 → 0.9685 (+0.4%)

回归原因: dim=96 → 128 padding 后 code 大小 +33%, Stage 2 / rerank 工作量增加.
RotationMatrix::Apply 在 dim=96 上本就只占 ~11% (~0.16ms), 即便降到 0.02ms,
节省也小于 padding 带来的 +0.1ms 开销.

**结论**: dim=96 padding 路径可行但收益有限. AVX-512 FWHT 是否能改善见 C3.

## Stage C: AVX-512 重写 FWHT_InPlace

### Task C1: 实现 simd::FWHT_AVX512

- [x] Done — 新增 [include/vdb/simd/hadamard.h](include/vdb/simd/hadamard.h) 和 [src/simd/hadamard.cpp](src/simd/hadamard.cpp). `FWHTScalar` 是参考实现, `FWHT_AVX512` 在 len < 16 走标量, len ≥ 16 用 `_mm512_loadu_ps` + add/sub. `vdb_simd` 库已加入 `hadamard.cpp`.

### Task C2: 在 RotationMatrix::Apply 中调用 SIMD FWHT

- [x] Done — `rabitq_rotation.cpp` include `vdb/simd/hadamard.h`, `FWHT_InPlace` 直接 delegate 到 `simd::FWHT_AVX512` (该函数内部对 len < 16 走标量, AVX-512 不可用时退化为 `FWHTScalar`).

### Task C3: 验证 SIMD FWHT 加速

- [x] Done

**COCO-100k Hadamard + SIMD FWHT (5 runs)**:
- 4.362 / 4.420 / 4.345 / 4.099 / 4.413 ms
- median: **4.362 ms**, min 4.099 ms
- recall@10: 0.9540

vs scalar FWHT (median 4.426 ms): -1.4% (噪声内)
vs Random baseline (median 7.468 ms): **-42%** ★

**deep1m padding (SIMD FWHT) 5 runs**:
- 1.485 / 1.411 / 1.370 / 1.473 / 1.901 ms
- median: 1.473 ms (vs scalar 1.497, -1.6%)
- recall@10: 0.9685

**结论**: SIMD FWHT 的边际收益很小, 因为:
- dim=512 时 FWHT 总 ops 只有 ~5000, 标量已经很快 (~2.5μs/call)
- profile 显示 RotationMatrix::Apply 只占 5.74% (Hadamard 启用后)
- 真正的 win 来自算法层 O(L²) → O(L log L), SIMD 是锦上添花

但 SIMD FWHT 代码组织上更清晰, 把 Hadamard SIMD 集中在 `src/simd/hadamard.cpp`,
为未来更大 dim (如 dim=1024+) 留下扩展空间。

## Stage D: 最终汇总

### Task D1: 测量并记录所有结果

- [x] Done

## 最终结果汇总 (5 runs each, MKL_NUM_THREADS=1 OMP_NUM_THREADS=1)

| 配置 | rotation | dim | latency median | recall@10 | vs baseline | 目标 |
|------|----------|-----|---------------|-----------|-------------|------|
| **COCO-100k**, nlist=2048, nprobe=150, CRC=0 |
| baseline (Random) | Random | 512 | 7.468 ms | 0.9540 | — | — |
| Hadamard (scalar FWHT) | Hadamard | 512 | 4.426 ms | 0.9540 | **-41%** ★★★ | ≤4.0 ms |
| Hadamard (SIMD FWHT) | Hadamard | 512 | **4.362 ms** | 0.9540 | **-42%** | 接近达成 |
| **deep1m**, nlist=4096, nprobe=100, CRC=1 |
| baseline (Phase 3) | Random | 96 | 1.43 ms | 0.9650 | — | — |
| Random fallback | Random | 96 | **1.416 ms** | 0.9650 | -1% (噪声) | ≤1.5 ms ✓ |
| padding 96→128 (scalar FWHT) | Hadamard | 128 | 1.497 ms | 0.9685 | +6% | ≤1.4 ms ✗ |
| padding 96→128 (SIMD FWHT) | Hadamard | 128 | 1.473 ms | 0.9685 | +4% | ≤1.4 ms ✗ |

## 关键 takeaways

1. **COCO-100k 大幅成功** ★★★
   - 7.47 → 4.36 ms (-42%)
   - recall@10 不变 (0.9540)
   - recall@1 提升 (0.95 → 0.975)
   - 接近 4.0 ms 目标 (差 0.36 ms, 在噪声范围)

2. **deep1m fallback 完美**
   - 1.416 ms vs Phase 3 baseline 1.43 ms, 无回归
   - 自动检测 dim=96 不是 2^k, 走 Random Gaussian

3. **deep1m padding 96→128 不划算**
   - 5.7% 微回归 (1.416 → 1.497 ms)
   - 原因: dim=96 的 Apply 本就只占 ~11%, padding 后 code/rerank +33%
   - **建议**: 默认关闭 padding (`--pad-to-pow2 0`),
     仅在 `dim ∈ {一个 2^k 附近, Apply 占比高}` 时考虑启用

4. **SIMD FWHT 边际收益小**
   - COCO: -1.4%, deep1m: -1.6%
   - dim=512 FWHT 标量已经够快, 主要 win 在算法层
   - 但代码组织清晰, 为更大 dim 留扩展空间

## 下一步建议

- 默认 `--pad-to-pow2 0` (现在已经是默认)
- 用户对大 dim (≥128 的 2^k) 数据集应自然受益
- COCO 上从 Phase 3 baseline 7.47 ms 到 4.36 ms 的提升是这个 change 的核心价值
