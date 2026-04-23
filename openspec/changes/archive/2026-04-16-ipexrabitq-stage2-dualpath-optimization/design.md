## Context

当前 clean perf 已经把统计污染压掉，query-only 口径下 `IPExRaBitQ` 成为最突出的单核热点。现有实现虽然已经有 AVX-512 / AVX2 SIMD，但它仍然偏向单 candidate、沿 dim 扫描的 decode-heavy 路径：每个 uncertain candidate 都会重复执行 packed-sign 解码、`u8 -> i32 -> f32` 幅值扩展和 query load。此前已经验证过，如果只是把 uncertain 先收集成 batch，但底层仍逐 candidate 调单 kernel，整体会退化，因为只引入了 batch 组织成本，却没有获得跨 candidate 的 query 复用。

因此这次 change 先收缩到一条明确路线：不改持久化格式，只做 Stage2 true multi-candidate kernel，并让 query-side 调用方式真正进入 batch kernel。

## Goals / Non-Goals

**Goals:**
- 细化 `IPExRaBitQ` 的 packed-sign true multi-candidate kernel 路线，明确实现边界、block 组织和正确性约束。
- 细化 Stage2 从逐 candidate 调用切换到 true batch kernel 所需的数据流、scratch 和调用边界。
- 继续保持 clean perf 与 full E2E 双口径，并能区分“单 candidate kernel”与“true multi-candidate kernel”的收益来源。

**Non-Goals:**
- 本次 design 不把存储格式升级作为前置条件。
- 第一阶段不改 recall、排序、CRC、payload、resident serving 语义。
- 不把 `EstimateDistanceFastScan`、coarse score、submit 链路的优化混入本 change。
- 不实现 rebuild-required 的 compact layout；若 true batch kernel 之后仍受 sign/code 带宽限制，再作为后续 change 单独判定。

## Decisions

### 决策一：当前 change 只做 true multi-candidate kernel，不做 compact layout / rebuild

实施顺序固定为：
1. packed-sign 单 candidate kernel 作为 reference fast path 保留
2. 实现 `batch=8` true multi-candidate kernel
3. 将 Stage2 调用点改成“收集 uncertain -> 调 true batch kernel”
4. perf 复盘

这样做的原因是：
- 当前最缺的不是 batch 调度外壳，而是真正跨 candidate 复用 query / block 计算的 kernel。
- 只要底层 kernel 仍是逐 candidate，任何 batch 外壳都很容易退化。
- compact layout / rebuild 会同时改 builder、store、parser、query path，复杂度太高，不适合作为当前 change 的第一步。

### 决策二：kernel 路线只优化 `packed_sign` 主路径

第一条路径 SHALL 默认面向当前正式 benchmark 主路径：`v10 + packed_sign + AVX-512`。内核接口可以继续保留通用入口，但实现上必须拆出 packed-sign 专用路径，不再要求主循环内同时兼容 byte-sign / packed-sign 两种模式。

packed-sign kernel 的设计边界：
- 按 64-dim block 作为主组织单位
- 每个 64-dim block 一次性读取 64 个 sign bit，再分发到 4 个 16-lane 子块
- `code_abs` 继续按当前 `uint8` 布局读取，不改持久化格式
- query 仍按常规 float 向量输入，不引入新的 query 预处理格式

备选方案：维持通用 `sign_packed ? packed_loader : unpacked_loader` 路线。
该方案实现更少，但在正式路径下会保留不必要的条件分派和较碎的 sign 解码。

### 决策三：kernel 路线采用 query-centric 计算结构，不再重建完整 signed code

后续 kernel 应围绕如下恒等变换组织：

```text
Σ q[i] * sgn[i] * (abs[i] + 0.5)
= Σ signed_q[i] * abs[i] + 0.5 * Σ signed_q[i]
```

实现上应优先做到：
- `sign` 只作用于 query 侧，得到 `signed_q`
- `dot` 与 `bias` 两部分分开积累
- 避免显式重建完整 `signed_code` float 向量

备选方案：继续围绕“解码成 signed float(code) 后做点积”的思路微调。
该方案不容易根治当前 decode-heavy 问题。

### 决策四：Stage2 必须进入 true batch kernel，而不是 batch 外壳包单 kernel

当前 change 里的 Stage2 目标结构固定为：
- 当前：`for each uncertain candidate -> IPExRaBitQ(...)`
- 目标：`collect uncertain -> IPExRaBitQBatchPackedSign(..., batch=8, ...)`

这里的关键约束是：
- batch kernel 必须在同一个 dim block 内同时累积多个 candidate
- query block 必须在 batch 内复用
- 不允许退化成“收集 batch 后再循环调用 8 次单 candidate kernel”

备选方案：保留 batch 外壳，再逐 candidate 调单 kernel。
该方案已被实验验证会退化，因此不再作为当前 change 的可接受实现。

### 决策五：query-side 先新增 batch scratch / pointer-span 视图，不改磁盘格式

当前 change 允许新增：
- Stage2 batch candidate scratch
- batch `code_abs_ptrs` / `sign_ptrs` / `xipnorm` / 元数据 span
- 便于 batch kernel 消费的 query-side 临时视图

当前 change 不做：
- compact layout
- rebuild-required 的新 cluster store 格式
- `ParsedCluster` 的磁盘布局升级

只有在 true batch kernel 完成后，若 perf 仍显示 Stage2 受限于 sign/code 带宽，才单独进入 compact layout / rebuild 路线。

## Risks / Trade-offs

- [packed-sign kernel 收益小于预期] → 以 clean perf 为准做决策；若 `IPExRaBitQ` 仍是主热点，则立即进入第二条路径而不是继续在单 kernel 上过拟合。
- [默认只优化 packed-sign 主路径会让 byte-sign 路径继续较慢] → 正式 serving 口径已经稳定在 v10 packed-sign，byte-sign 只保留兼容路径，不作为主优化目标。
- [true batch kernel 实现复杂度高] → 固定第一版 `batch=8`，只支持 packed-sign 主路径，尾批次和非 packed-sign 走兼容回退。
- [benchmark 归因更难] → 继续保持 clean perf 与 full E2E 双口径，并单独回填“单 candidate kernel”与“true batch kernel”前后的结果。
- [stage2 batch scratch 过大或引入额外拷贝] → 限制第一版 batch 大小，并优先复用 scheduler / query wrapper 级 scratch，而不是在 candidate 循环中分配临时对象。

## Migration Plan

1. 建立本 change，并明确两条路径与决策门。
2. 实现 packed-sign true multi-candidate kernel，保持当前 v10 存储格式不变。
3. 将 Stage2 调用点切到 true batch kernel。
4. 跑 clean perf 与 full E2E，对比 `IPExRaBitQ` 占比和 `avg_query`。
5. 若 true batch kernel 之后 Stage2 仍明显受 sign/code 带宽限制，再另开 compact layout / rebuild change。

## Open Questions

- true batch kernel 第一版内部并行是 `8-way` 还是 `4-way` 更利于当前 AVX-512 寄存器压力。
- `IPExRaBitQ` packed-sign true batch kernel 是否需要显式 lane-mask 输入，还是由调用侧只传有效 candidate count。
- 如果后续进入更激进的数据布局，是否直接靠近 RaBitQ-Library 的 `ExDataMap` / compact ex-code 组织，还是保持更贴近当前 cluster store 的本地布局。
