## Context

当前 resident 主工作点为 `full_preload + use_resident_clusters=1 + crc=1 + nprobe=256 + bits=4`。在该工作点上，查询几乎会完整走满 `nprobe`，因此每个 probed cluster 的固定 CPU 成本会被线性放大。最新 benchmark 细粒度统计显示：

- `probe_prepare_ms` 明显高于 `probe_stage1_ms` 与 `probe_submit_ms`
- `probe_prepare_ms` 内部主要集中在 `prepare_quantize_ms` 与 `prepare_lut_build_ms`
- `stage1_mask / iterate / classify` 已经较薄，Stage1 内真正仍然较重的是 `EstimateDistanceFastScan`

这说明当前主瓶颈已经不是 CRC 判停逻辑，也不是 coarse 或 I/O，而是 resident per-cluster FastScan prepare 还没有收敛成足够薄的 hot path。现有 fused prepare 路径虽然在接口上合并了 `quantize + lut_build`，但实现上仍然保留了 chunk 级中间态、group 级小粒度循环和 repeated scratch state maintenance。与此同时，后续实验不会只停留在当前数据集和 512 维工作点，还需要覆盖 `96 / 1024 / 3072` 以及非 `2^n` 维度，因此本轮设计必须优先追求跨维度稳定收益，而不是把单一维度 specialization 误当成主方案。

## Goals / Non-Goals

**Goals:**
- 进一步压缩 resident per-cluster prepare 固定成本，优先降低 `probe_prepare_ms`
- 将 fused FastScan prepare 改造成更接近单遍流式生成的 hot path，减少量化结果的中间落地与重复读取
- 在 resident hot path 下加强 `PreparedQuery` / scratch 的固定容量复用，减少每 cluster 的容器状态维护
- 优先拿到对不同维度都成立的 prepare hot-path 收益，并保留后续按 profiling 决定是否增加 specialization 的空间
- 保留并加强现有 benchmark 观测能力，使收益可以继续映射到 `prepare_subtract / normalize / quantize / lut_build`

**Non-Goals:**
- 不修改 CRC early-stop 判停逻辑、标定逻辑或 frontier 语义
- 不改变 recall、candidate class 语义、最终 rerank 或 payload 语义
- 不在本轮重写 Stage2 kernel 或新的存储格式
- 不把任意固定维度 specialization 作为本轮 change 的核心实现承诺

## Decisions

### 决策一：把优化重心放在 prepare，而不是继续优先挖 Stage1 classify 或 submit

benchmark 已显示 prepare 是当前主工作点上的最大 query 子项，因此本轮设计优先处理 `PrepareQueryRotatedInto()` 与 fused FastScan prepare，而不是继续优先优化 Stage1 的 `mask / iterate / classify` 或 submit 路径。

选择原因：
- `probe_prepare_ms` 在主工作点上是最大项
- Stage1 细分中 `mask / iterate / classify` 已经较薄
- submit/I/O 基本已不再是主要瓶颈

备选方案：
- 继续优先优化 `EstimateDistanceFastScan` 或 Stage2 kernel
  该方案有潜在收益，但与当前实测主瓶颈不匹配，因此不作为第一阶段重点

### 决策二：fused FastScan prepare 采用“更真融合”的单遍生成策略

实现上，`QuantizeQuery14BitWithMaxToFastScanLUT()` 将继续保留 fused 入口，但内部不再满足于“先生成 chunk 量化结果，再 group-by-group 构建 LUT”的弱融合结构。设计目标是让量化后的 4-dim group 尽可能直接进入 LUT 生成与 packed write，减少 `q_chunk` 一类中间态 materialization。

选择原因：
- 细粒度统计表明 `prepare_quantize_ms` 与 `prepare_lut_build_ms` 是 prepare 的主要组成
- 当前 fused 路径仍存在较多中间态和小粒度循环
- 这是最直接影响 `probe_prepare_ms` 的技术抓手

备选方案：
- 只在现有 chunk 结构上做轻微 unroll
  这种方案开发成本低，但大概率只能拿到有限收益，无法解决“逻辑上 fused、实现上仍分段”的根本问题，因此不作为主方案

### 决策三：resident hot path 下的 scratch 与 PreparedQuery 采用固定容量复用

在 resident 主路径下，query wrapper、`PreparedQuery` 和 `ClusterPreparedScratch` 将被视为长生命周期 hot-path state。设计要求 steady state 下避免 repeated `resize`、对齐重算和不必要的清零；查询期只覆盖内容，不重复维护可以前移的容器状态。

选择原因：
- 当前每 cluster prepare 仍包含明显的容器状态维护
- resident hot path 已经是稳定的专用路径，适合强化固定容量复用
- 该优化风险低，且与 FastScan fused prepare 优化互补

备选方案：
- 保持当前 vector-based state，仅依赖 reserve 避免 realloc
  该方案无法消除热路径上的 size/state 维护，因此不采用

### 决策四：优先构建跨维度稳定受益的通用 hot path，specialization 作为可选增强后置

本轮设计不把 `dim=512` 或其他固定维度快路径作为核心承诺。主方案必须先在不同维度上都成立，包括非 `2^n` 维度和较大维度；只有在通用路径优化完成后，且 profiling 明确支持时，才考虑增加受控 specialization。任何 specialization 都必须是可回退的附加层，而不是唯一实现路径。

选择原因：
- 后续实验会覆盖多个数据集和不同维度
- 单一维度最优不等于方法层面的长期最优
- 先把通用路径做薄，才能更干净地判断 specialization 是否真正值得引入

备选方案：
- 围绕当前主工作点直接加入 `dim=512` 专用快路径
  该方案可能对单点 benchmark 有利，但会抬高跨维度维护和回归风险，因此不作为本轮主方案

当前实现阶段的结论是：先不在本 change 中引入 specialization。实测 generic path 已经在 `coco_100k` 当前工作点上显著降低 `probe_prepare_ms`，并且在 `deep1m` 的原生 `dim=96`（未启用 `pad-to-pow2`）向量搜索路径上正常工作，因此当前没有足够证据表明固定维度特化是必要的。

### 决策五：保留可观测性，不以“变黑盒”换取实现自由

本轮优化后，benchmark 仍必须能够稳定报告 `probe_prepare_ms` 以及 `prepare_subtract / normalize / quantize / lut_build` 子项。设计上不允许通过合并字段或移除边界来掩盖回归或收益来源。

选择原因：
- 这轮优化需要持续判断收益究竟来自 quantize、LUT build、scratch 管理还是其他子段
- 如果失去现有边界，后续 Stage1/Stage2 的调优就会重新失焦

备选方案：
- 仅保留总 prepare 时间
  该方案会削弱后续优化判断能力，因此不采用

## Risks / Trade-offs

- [风险] 更真融合的 FastScan prepare 可能增加实现复杂度，并引入与参考路径不一致的输出风险。  
  → Mitigation：保留 reference-equivalence 校验路径，并确保 `fs_width / fs_shift / packed LUT bytes` 一致。

- [风险] 过早引入固定维度 specialization 可能导致其他维度收益不稳定，甚至引入维护分叉。  
  → Mitigation：先完成通用路径优化，只把 specialization 作为 profiling 支持下的后续可选层，并始终保留通用回退路径。

- [风险] 去掉 repeated resize / 清零后，若覆盖写不完整，可能产生脏数据污染。  
  → Mitigation：要求相关缓冲满足“完整覆盖写”或仅对尾部保留显式 mask/清理。

- [风险] benchmark 细粒度 timing 本身会抬高绝对时延。  
  → Mitigation：将细粒度 timing 用于归因，不作为最终绝对性能口径；最终验收仍以低开销统计和同参数 benchmark 为准。

## Migration Plan

1. 先补齐本 change 的 spec delta，明确 fused prepare、resident scratch 复用和观测边界要求。
2. 第一阶段实现低风险优化：固定容量 scratch / wrapper、对齐缓存前移、减少 repeated resize 与清零。
3. 第二阶段重构 fused FastScan prepare：缩短 `quantize + lut_build` 中间态路径，并把 chunk 宽度等策略保持为通用路径可调内部参数。
4. 第三阶段在代表性维度上复测通用路径，包括非 `2^n` 维度，确认收益是否跨维度稳定。
5. 只有在 profiling 明确支持时，才进入可选 specialization 阶段，并保持通用回退路径。
6. 若某阶段出现 recall、等价性或跨维度回归问题，优先回退对应 specialization 或 fused path 重构，不回退已验证的 scratch 复用改动。

## Open Questions

- `BuildLutAndStoreGroup` 是否应继续保留现有接口并依赖调用方内联，还是后续仍值得拆成更强的 fused-only helper。
- `kGroupsPerChunk` 的稳健默认值在更大维度（如 1024 / 3072）上是否仍与当前一致，还是需要继续作为通用路径内部可调参数。
- 若后续 Stage2 成为新的主瓶颈，是否需要补充 `collect / kernel / scatter` 级 timing 来继续细分收益来源。
