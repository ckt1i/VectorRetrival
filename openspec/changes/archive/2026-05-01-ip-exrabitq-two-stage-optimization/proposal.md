## Why

现有 clean perf 已经表明，`prepare_lut_build` 与 Stage1 `iterate/classify` 在前几轮优化后不再是 query 主路径的第一热点，新的纯计算主热点已经收敛到 Stage2 的 `IPExRaBitQ`。如果继续只围绕 FastScan prepare 或 Stage1 打磨，后续收益会被 `code_abs + sign -> signed float -> dot` 这条 decode-heavy 路径吃掉，因此需要单独开一轮 change，把 `IPExRaBitQ` 的两阶段优化路线设计清楚。

## What Changes

- 新增一份专门面向 `IPExRaBitQ` 的两阶段优化 change，明确目标是在 `AVX-512 + resident + IVF` 场景下，以跨数据集、跨维度稳定收益为优先，而不是只针对单一 `dim` 写特化 kernel。
- 第一阶段固定为“不改索引格式”的 kernel 重写：
  - 保持当前 `code_abs[dim bytes] + sign[dim bytes] + xipnorm` 布局不变
  - 将 `IPExRaBitQ` 从“重建 signed float(code) 再点乘”的 code-centric 路径改成“幅值与符号分离消费”的 query-centric AVX-512 kernel
  - 要求对不同常见维度（如 96/128/256/384/512/768/1024/1536）保持正确性与稳定性能
- 第二阶段固定为“按收益决定是否进入存储格式升级”：
  - 若第一阶段后 `IPExRaBitQ` 仍是主热点，则继续评估 `sign bit-pack`
  - 若 `sign bit-pack` 后仍受限，再进入 `compact ex-code layout` 设计
  - 明确这两步属于第二阶段增强，不作为第一阶段的前置依赖
- 补充 `IPExRaBitQ` 专项 perf / benchmark 验收口径：
  - clean perf 必须去除 prepare 细粒度打点污染
  - full E2E 与 query-only 两个口径都要回填
  - 必须能区分“kernel 真变快”与“打点/submit 污染导致的假回退”
- 固定后续实现边界：
  - 第一阶段不改 recall / 排序 / CRC / payload / resident serving 语义
  - 第一阶段不改 cluster store 持久化格式
  - 第二阶段若进入 bit-pack / compact layout，必须以兼容迁移路径或显式 rebuild 为前提

## Capabilities

### New Capabilities
- `exrabitq-stage2-kernel`: 定义 `IPExRaBitQ` 的阶段化优化路径、kernel 契约、跨维度处理边界，以及第一阶段和第二阶段之间的决策门。

### Modified Capabilities
- `query-pipeline`: query 主路径要求需要补充 Stage2 `IPExRaBitQ` 优化后的执行边界、热点归因与后续决策规则。
- `e2e-benchmark`: benchmark 要求需要继续支持 clean perf 与 full E2E 双口径，并明确 `fine_grained_timing=0` 时不得把 prepare 细粒度打点混入 perf 主路径。

## Impact

- 主要影响 [src/simd/ip_exrabitq.cpp](/home/zcq/VDB/VectorRetrival/src/simd/ip_exrabitq.cpp)、[include/vdb/simd/ip_exrabitq.h](/home/zcq/VDB/VectorRetrival/include/vdb/simd/ip_exrabitq.h)、[src/index/cluster_prober.cpp](/home/zcq/VDB/VectorRetrival/src/index/cluster_prober.cpp) 以及与 query-only perf / full E2E 结果回填相关的 benchmark 路径。
- 第一阶段不引入新外部依赖，不改变现有索引格式和 cluster 持久化格式；第二阶段若进入 `sign bit-pack` 或 `compact ex-code layout`，会影响编码、存储、加载与重建流程。
- 该 change 的输出重点不是立刻大改存储格式，而是先建立一个“先 kernel、后 layout”的两阶段实施蓝图，避免为了单一数据集或单一维度过拟合实现。
