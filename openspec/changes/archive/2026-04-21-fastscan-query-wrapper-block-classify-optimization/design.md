## Context

最近一轮 `full_preload + use_resident_clusters=1 + nprobe=64 + crc=1` 的 full E2E 与 clean perf 已经把主路径重新排序了。当前 query 时间的主要热点不再是 coarse top-n 或 rerank，而是：

- `PrepareQueryRotatedInto`
- `BuildFastScanLUT`
- Stage1 的逐 lane `iterate/classify`
- `AsyncIOSink::OnCandidate` / `PrepReadTagged`

这说明当前系统的主要问题不是某一个 SIMD kernel 太慢，而是 query 主路径仍然偏“细粒度驱动”：

```text
query
 ├─ per-cluster PrepareQueryRotatedInto
 │   ├─ diff + norm
 │   ├─ normalize + sign + sum
 │   ├─ quantize
 │   └─ LUT build
 └─ per-block Stage1
     ├─ EstimateDistanceFastScan
     ├─ SafeOut mask
     └─ while (maybe_in)
         ├─ classify one lane
         ├─ decode one address
         └─ submit one candidate
```

要继续往下压时延，必须先把 prepare 和 Stage1 改造成 batch-friendly 的结构，让后续 SIMD 或更深层 kernel 优化有稳定落点。与此同时，benchmark 需要继续同时支持两种口径：

- `full E2E`：用于真实 recall / latency 验收
- `query-only clean perf`：用于低干扰热点分析

## Goals / Non-Goals

**Goals:**
- 把 resident 查询路径中的 query-global、cluster-local 和 FastScan-ready 状态拆成更清晰的 query wrapper / prepared view 结构。
- 将 `PrepareQueryRotatedInto` 进一步收敛成面向 FastScan 的 prepare 流程，并保留对后续 fused `quantize + LUT build` 的接口空间。
- 把 Stage1 从逐 lane 控制流改造成 block-driven candidate compaction 与 batch submit。
- 为 resident + single assignment 路径建立 batch candidate submit 的正式边界，包括批量地址解码和批量 sink 接口。
- 在设计中明确两步优化完成后哪些路径已经稳定到适合继续做 SIMD。

**Non-Goals:**
- 不在本轮改变 CRC 语义、payload 语义或 batch rerank 语义。
- 不直接切换到 `RaBitQ-Library` 作为 serving 实现，只借鉴其 query wrapper / batch scan 的组织方式。
- 不修改索引文件格式，不改变 clustering / assignment 语义。
- 不在本轮默认引入 MKL/GEMV 作为主路径依赖。

## Decisions

### 决策一：把 `PreparedQuery` 拆成 query wrapper 驱动的两层结构

实现上，当前 `PreparedQuery` 将被逻辑上拆成两类状态：

- `ResidentQueryWrapper`
  - `rotated_q`
  - 可复用 diff / sign / quant / LUT scratch
  - query-only 常量与 profiling 配置
- `PreparedClusterQueryView`
  - `norm_qc`
  - `norm_qc_sq`
  - `sum_q`
  - `fs_width`
  - `fs_shift`
  - 指向 scratch 中当前 cluster 结果的 view

目标不是暴露更多公共 API，而是让 resident query path 的生命周期更清晰：

```text
Search()
 ├─ once-per-query
 │   └─ ResidentQueryWrapper
 └─ per-cluster
     └─ PreparedClusterQueryView
```

这样做的原因是当前热点已经表明，prepare 的成本不只是算子本身，还包括对象边界不清导致的中间态保留和重复搬运。参考 `RaBitQ-Library::SplitBatchQuery` 的做法，query path 更适合由 wrapper 驱动，而不是让 `PreparedQuery` 继续承担所有阶段的长期语义。

备选方案：
- 保持 `PreparedQuery` 结构不变，只做更多 reserve / inline 微调。
  这个方案改动最小，但无法从结构上支持下一步的 fused prepare 和 block-driven classify，因此不采用。

### 决策二：`PrepareQueryRotatedInto` 采用“两段 prepare + 一处稳定出口”

`PrepareQueryRotatedInto` 的目标流程固定为：

```text
Stage A: cluster residual prepare
  rotated_diff = rotated_q - rotated_centroid
  norm_qc_sq
  norm_qc

Stage B: fastscan-ready prepare
  normalize
  sign_code
  sum_q
  max_abs
  quantize
  LUT build

Output:
  PreparedClusterQueryView
```

第一轮实现不要求立刻做最激进的 fused `quantize + LUT build`，但要求：

- 所有 prepare 子步骤的责任边界清晰
- 当前优化后的 `normalize/sign/sum/max_abs` 融合继续保留
- FastScan-ready 结果通过稳定的 prepared view 输出

这样做的原因是下一轮真正可能继续做 SIMD 的地方，不是抽象的“prepare”，而是：

- `diff + norm`
- `normalize/sign/sum/max_abs`
- `quantize`
- `LUT build`

如果不先把这几个阶段固定下来，后续就无法判断该把 SIMD 投入在哪一层。

备选方案：
- 直接在本轮把 `quantize + BuildFastScanLUT` 完全融合成单一大函数。
  这个方向长期上是合理的，但当前还缺少稳定的数据边界和更干净的回归面。先固定两段 prepare 与 prepared view，再做更深融合更稳妥，因此本轮不把它设为强制项。

### 决策三：Stage1 改成 block-driven classify，而不是继续优化逐 lane 循环

新的 Stage1 路径将按 block 执行：

```text
for each fastscan block:
  1. EstimateDistanceFastScan
  2. FastScanSafeOutMask
  3. compact survivors into block-local arrays
  4. classify safein / uncertain in batch
  5. batch decode addresses
  6. submit candidate batch to sink
```

也就是从当前：

```text
while (maybe_in):
  classify one lane
  AddressAt(one lane)
  OnCandidate(one lane)
```

迁移到：

```text
CandidateBatch
  ├─ global_idx[<=32]
  ├─ est_dist[<=32]
  ├─ class[<=32]
  └─ decoded_addr[<=32]
```

这样做的原因是 perf 和细分计时都表明，`mask` 很轻，重的是 lane 之后的一连串碎操作。真正值得做的不是把 `ctz` 再抠快一点，而是把控制流粒度从“每 lane 一次”提升到“每 block 一次”。

备选方案：
- 继续保留单候选接口，只对 classify 内核做 SIMD。
  这个方案会让 `classify` 变快一些，但 `AddressAt` 和 `OnCandidate` 的碎粒度成本还在，因此不采用。

### 决策四：为 `ParsedCluster` 和 sink 增加 batch 接口

为了支撑 block-driven classify，需要增加两个稳定批量接口：

- `ParsedCluster::DecodeAddressBatch(...)`
- `ProbeResultSink::OnCandidateBatch(...)`

第一版不强制要求 `DecodeAddressBatch` 使用 SIMD，但必须把它变成明确的批量边界。后续如果实现证明其已经成为稳定热点，则它会是下一轮 SIMD 候选。

这样做的原因是后续真正适合继续做 SIMD 或数据布局优化的地方，必须先是“批量稳定点”，而不能还是隐藏在逐对象 API 里的碎路径。

备选方案：
- 在 `ClusterProber` 内部直接批量 decode 地址，但不暴露正式接口。
  这个方案短期可行，但会把后续 SIMD 或 layout 优化锁死在局部实现里，因此不采用。

### 决策五：在设计中明确“第二轮可继续 SIMD 化”的稳定路径

这次改造完成后，以下路径被认为是“已批量化或已定型，适合下一轮继续 SIMD”的候选：

1. `PrepareQueryRotatedInto` 的 `quantize + LUT build` 融合路径
2. Stage1 中 `safein` 的 block mask 生成与 compaction
3. `DecodeAddressBatch` 的批量解码路径
4. batch sink 内按 class 分组后的 slot prep / submit 前整理
5. `EstimateDistanceFastScan` 之后的 block-local classify 辅助计算

设计上要求把这些路径当作正式的“后续 SIMD 接口面”记录下来，而不是普通注释。这样在本轮实现完成后，可以直接基于新结构重新做一轮 perf，再决定下一轮 SIMD 的优先级。

### 决策六：E2E 与 clean perf 继续并行存在，但口径职责分离

benchmark 继续保留：

- `--fine-grained-timing 1`
  - 用于 full E2E 结果解释
  - 输出 Stage1 细分字段
- `--fine-grained-timing 0`
  - 用于 clean perf
  - 减少 `steady_clock` 干扰

这样做的原因是这次 change 的目标本来就是先把结构理顺再做更深优化。没有干净 perf，就无法判断新结构下的真实热点；没有细粒度 E2E 字段，又无法判断收益落在哪个子阶段。

### 决策七：prepare 边界清理作为下一步推进线

在确认 Stage1 batch 化后，prepare 结构继续往前推进为“query-wrapper 主导、view 约束消费”。具体目标是：

- 第一级（Query 全局）：`rotated_q` 与复用 scratch 统一在 query wrapper 内管理，作为唯一 per-query 常驻内存源。
- 第二级（Cluster 局部）：`PrepareClusterResidual` 只计算 `diff + norm_qc_sq + norm_qc + inv_norm`，只产出临时 residual view。
- 第三级（FastScan 视图）：`PrepareFastScanView` 只从 residual view 产出最小化 `PreparedClusterQueryView`，包括 `sum_q / fs_width / fs_shift / sign_code / lut` 所需引用。

这三层分离后，`ClusterProber::Probe` 仅依赖 view，不能再依赖完整 `PreparedQuery` 生命周期。该调整的直接价值不是一次性大幅降时，而是把 `PrepareQueryRotatedInto` 的后续可继续优化空间收敛为可切换模块。

### 决策八：prepare 与 Stage1 的指标边界定义

为避免收益归因失真，本轮将 prepare/stage1 计时口径约束为：

- `prepare` 成本必须可对应到 “query-wrapper prepare / per-cluster residual / fastscan view” 三段。
- `probe_stage1` 必须只吸收 `EstimateDistanceFastScan + mask + classify` 的统计，不得把 submit 相关 CPU 计入 stage1 内部。
- `submit` 及地址/IO 提交流程在单独字段中报告，保持后续“结构优化”与“系统开销”可分离判断。

## Risks / Trade-offs

- [风险] query wrapper / prepared view 的逻辑拆分会增加结构复杂度。  
  → Mitigation：保持 public API 最小变化，优先在内部逻辑上分层，并通过 spec 固定每层责任。

- [风险] block-driven classify 容易在边界条件上引入结果回归。  
  → Mitigation：要求 `safeout / safein / uncertain` 的最终语义与参考路径一致，并把 compact 后的 batch 只当作组织方式变化，而不是语义变化。

- [风险] batch 接口引入后，如果内部仍然逐元素实现，可能短期收益有限。  
  → Mitigation：接受第一轮先把结构搭起来，把收益判断延后到 clean perf；本轮重点是建立稳定的优化落点。

- [风险] `DecodeAddressBatch` 可能会暴露现有地址布局本身不利于批量化的问题。  
  → Mitigation：先建立批量 API，必要时在下一轮再单独处理 address layout 或 SIMD decode。

- [风险] full E2E 和 clean perf 的数字可能不可直接横向比较。  
  → Mitigation：在 benchmark 中明确 `fine_grained_timing` 配置，并把两种口径分别用于解释和 profiling，而不是混用。
