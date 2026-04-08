## Why

当前 ProbeCluster 同时计算 FastScan 距离和 Popcount 距离，但 S1 分类只用 Popcount。bench_vector_search 实验（deep1m, bits=4）验证：用距离误差校准的 FastScan 直接分类可以消除整个 Popcount 循环，recall 不变 (0.966→0.965)，延迟 -27% (3.14ms→2.29ms)。

根本原因：距离误差的主要来源是数据库侧 1-bit 量化（eps_ip_fs ≈ eps_ip_pop ≈ 0.17），不是查询侧精度。FastScan 的 batch-32 VPSHUFB 替代逐向量 UnpackSignBits + PopcountXor，纯粹是计算效率提升。

## What Changes

- **IvfBuilder**: `CalibrateEpsilonIp` 改为距离误差校准（`|dist_fastscan - dist_true| / (2·norm_oc·norm_qc)`），替代当前的 IP 误差校准
- **OverlapScheduler::ProbeCluster**: 删除 Popcount 循环（UnpackSignBitsFromFastScan + PopcountXor + pop_dists 计算），直接用 FastScan dists[] 做 S1 分类和 CRC est_heap 更新
- **CRC CalibrateWithRaBitQ**: 用 FastScan 距离替代 popcount 距离，保持校准和搜索的一致性

## Capabilities

### New Capabilities
- `fastscan-classify`: ProbeCluster 用 FastScan 距离直接做 S1 分类，消除 Popcount 循环
- `distance-error-calibration`: eps_ip 校准改为距离误差方法

### Modified Capabilities

## Impact

- **代码**: `src/index/ivf_builder.cpp` (~60行改 calibration), `src/query/overlap_scheduler.cpp` (~50行删 popcount 循环 + 改分类逻辑), `src/index/crc_calibrator.cpp` (~30行改 CRC 距离计算)
- **性能**: 预期 -27% probe 延迟（bench_vector_search 实验验证）
- **兼容性**: 新建索引的 eps_ip 值会略有不同（距离误差校准 vs IP 误差校准），但数值相近。旧索引仍可用（eps_ip 保守，不会引起 false SafeOut）
- **Benchmark**: bench_vector_search --use-fastscan 1 已验证可行性
