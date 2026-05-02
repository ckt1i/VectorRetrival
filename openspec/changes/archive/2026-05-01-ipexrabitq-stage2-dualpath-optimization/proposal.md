## Why

现有 clean perf 已经把 query 主路径热点压缩到少数几个纯计算核上，其中 `IPExRaBitQ` 已成为最大的单核热点。此前已经验证过“只加 batch 调度壳、底层仍逐 candidate 调单 kernel”的做法会退化，因此当前 change 需要先收缩范围，只保留不改持久化格式的 Stage2 true multi-candidate kernel 路线，先验证 query-side 真正跨 candidate 复用能否带来收益。

## What Changes

- 将当前 change 收缩为一条可独立实施、可逐轮验收的 Stage2 kernel 路线。
- 路线固定为 `packed_sign` 专用 true multi-candidate kernel 优化：
- 保持当前 v10 存储格式与 cluster store 持久化格式不变。
- 将 `IPExRaBitQ` 从当前 packed-sign 单 candidate kernel，演进为 `batch=8` 的 true multi-candidate kernel。
- 保持 Stage1 / candidate funnel / resident serving 语义不变。
- 将当前 `for each uncertain candidate -> IPExRaBitQ(...)` 的逐 candidate 调用方式，演进为“先收集 uncertain candidates，再交给 true batch kernel”。
- query-side 允许新增 batch scratch 与 batch pointer/span 视图，但不引入 rebuild-required 的 compact layout。
- 为当前路径建立统一的 perf 与 benchmark 决策门：
- clean perf 必须继续使用 `query-only + fine_grained_timing=0`。
- full E2E 继续使用 resident + full_preload + crc 口径。
- 必须能区分“单 candidate packed-sign kernel”与“true multi-candidate kernel”各自带来的收益。
- compact layout / rebuild 不在当前 change 内实施，只作为后续候选路线记录。

## Capabilities

### New Capabilities
- `ipexrabitq-kernel-optimization`: 定义 `IPExRaBitQ` packed-sign true multi-candidate kernel 的执行边界、数据消费方式、block 策略和正确性约束。

### Modified Capabilities
- `query-pipeline`: query 主路径需要新增 Stage2 true batch kernel 路线的执行边界、统计口径和语义不变约束。
- `e2e-benchmark`: benchmark 需要继续支持 clean perf 与 full E2E 双口径，并明确如何观察 true multi-candidate kernel 的收益。

## Impact

- 主要影响 `src/simd/ip_exrabitq.cpp`、`include/vdb/simd/ip_exrabitq.h`、`src/index/cluster_prober.cpp`、cluster parse / ex-data 访问路径，以及 benchmark / perf 回填逻辑。
- 当前 change 不引入新外部依赖，不改变索引格式。若需要新增 batch-friendly ex-data 视图或 scratch，只影响 query 侧数据组织。
- 更激进的 compact layout / rebuild 会另开后续 change，不和本轮 true batch kernel 混做。
