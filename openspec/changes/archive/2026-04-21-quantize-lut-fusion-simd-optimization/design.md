## Context

当前 query 主路径已经能稳定输出 `prepare_subtract`、`prepare_normalize`、`prepare_quantize`、`prepare_lut_build` 以及 Stage1 的 `estimate/mask/iterate/classify` 子项。最近一轮 `coco_100k / nlist=2048 / nprobe=64 / full_preload / resident=1 / crc=1` 的结果显示：

- `prepare_lut_build` 约 `0.078 ms`，明显大于 `subtract / normalize / quantize`
- Stage1 中 `mask` 很轻，主要成本仍在 `iterate + classify`

这说明当前系统在 prepare 阶段的主要问题不是“某个标量算子慢”，而是 `normalized query -> quant_query -> BuildFastScanLUT -> packed layout` 这条路径的中间态 materialize 与布局转换成本偏高。与此同时，Stage1 的后半段已经从“逐候选提交”升级为 batch submit，但 survivor 提取与分类仍然偏碎。此次 change 的目标是把 `quantize + lut_build` 融合路线和下一轮 SIMD 清单一次性设计完整，作为后续实现蓝图。

## Goals / Non-Goals

**Goals:**

- 定义 `quantize + lut_build` 的完整融合优化方案，并给出推荐实现顺序。
- 明确保留两段版与融合版之间的功能等价条件，包括 `fs_width`、`fs_shift`、packed LUT bytes 和下游 FastScan 距离等价。
- 固定 `BuildFastScanLUT` 的下一轮 SIMD 落点，包括 group-level LUT 生成、byte-plane 生成和最终 packed 写出。
- 给出 prepare/stage1 内仍值得继续做 SIMD 的详细候选清单、优先级和依赖关系。
- 固定后续 benchmark / perf 验收口径，确保每轮优化都可与当前结果直接对比。

**Non-Goals:**

- 本 change 不直接实现 fused kernel，也不修改索引格式或 FastScan block 格式。
- 不改变 recall、排序、CRC、payload、resident serving 语义。
- 不引入 MKL、oneDNN 等新的外部依赖。
- 不把 Stage2 / rerank / payload 后段混入本 change 的主目标。

## Decisions

### 决策一：优先推进“融合版”，但保留“两段版”作为回归基线

后续实现将同时保留两条逻辑基线：

- 参考路径：`QuantizeQuery14BitWithMax -> BuildFastScanLUT`
- 优化路径：fused `quantize + lut_build`

这样做的原因是：

- `prepare_lut_build` 已经是 prepare 主热点，说明进一步优化需要缩短数据通路，而不是单独抠某个小循环。
- 完全替换旧路径会降低回归可解释性；保留参考路径有利于逐 query 验证 `fs_width / fs_shift / LUT bytes` 等价。

备选方案：
- 只优化现有 `BuildFastScanLUT` 标量实现，不设计融合路径。
  该方案实现较简单，但无法消除 `quant_query` materialize / reload 的双遍历成本，因此不作为主线。

### 决策二：融合版的目标不是消除 `quant_query` 语义，而是消除“完整中间态往返”

融合版 SHALL 允许局部小块或寄存器级量化中间态存在，但不再要求：

- 先完整写出 `quant_query[dim]`
- 再完整读回 `quant_query[dim]`
- 再做 LUT 展开与 layout 写回

推荐的数据流为：

```text
normalized rotated query
  -> block quantize (4/8/16 lanes)
  -> group-local LUT generation
  -> lo/hi byte plane split
  -> final packed LUT layout write
```

这样做的原因是当前热点更像 memory/layout dominated，而不是纯算术 dominated。

备选方案：
- 继续完整 materialize `quant_query`，仅在 `BuildFastScanLUT` 内部加少量 SIMD。
  该方案可能拿到小收益，但难以触及主瓶颈。

### 决策三：`BuildFastScanLUT` 的优化边界按“三段”划分

为便于后续实现和 profiling，`lut_build` 内部再细分为三段：

1. group-level 16-entry LUT generation
2. lo/hi byte plane generation
3. packed lane layout write-out

这三段将作为未来继续 SIMD 化和计时归因的稳定边界。

这样做的原因是当前的 `BuildLut16 + low/high byte split + packed write` 混在一起，不利于判断真正的布局转换瓶颈在哪里。

当前可以立即进入实现设计的 SIMD 点具体包括：

1. `BuildLut16` 的 16-entry group LUT 生成
   - 用 nibble mask 模板或固定组合模板，避免逐项标量展开 16 次
2. lo/hi byte plane 拆分
   - 直接对 16-entry LUT 做向量 pack / shift / mask，而不是先落完整 16-bit 再拆
3. packed lane layout 直写
   - 按 `AccumulateBlock()` 需要的最终布局直接写，避免额外中间格式
4. `QuantizeQuery14BitWithMax` 的 pack/store 压缩
   - 作为 fused 路径前半段，减少 `int32 tmp -> int16` 的额外中转

这些点里，1/2/3 属于 `prepare_lut_build` 主热点，4 属于 fused 路径的前端配套优化。

### 决策四：Stage1 下一轮重点不是 `mask`，而是 survivor 后处理

当前 Stage1 时间分解已经说明：

- `mask` 很轻
- `iterate + classify` 明显更重

因此下一轮 SIMD 清单中，Stage1 的优先点固定为：

1. block-level survivor compaction
2. batch classify
3. batch address decode
4. batch submit 前整理

而不是继续优先优化 `FastScanSafeOutMask`。

备选方案：
- 继续优先打磨 `mask` kernel。
  该方案不匹配现有 profiling 结果，因此不采用。

当前 Stage1 内可明确推进的 SIMD / batch 点包括：

1. survivor compaction
   - 从 `so_mask -> maybe_in -> ctz while-loop` 变成批量 survivor index 提取
2. batch classify
   - 对 compact 后 survivor 批量生成阈值并批量比较 `est_dist < threshold`
3. `EstimateDistanceFastScan` 后续压缩
   - 继续评估 `AccumulateBlock` 与 `FastScanDequantize` 的中间写回是否可减
4. `DecodeAddressBatch`
   - 保持 batch-first 语义，后续视 address layout 再决定 gather / layout 优化
5. batch submit 前整理
   - `SafeIn / Uncertain` 分组后的 slot prep、地址组织和提交前整理

其中 1/2 是当前最应该优先做的，3/4/5 是第二梯队。

### 决策五：不将 MKL 作为当前方案主线

本 change 记录为明确结论：当前热点不属于 GEMV/GEMM 型问题，主要是布局转换、压缩、mask 和 batch classify，因此不建议优先引入 MKL。

备选方案：
- 用 MKL 替代部分 prepare。
  该方案会增加依赖和复杂度，但对现有热点命中不准，因此不采用。

同样明确不建议优先投入的点包括：

- `FastScanSafeOutMask` 深挖
  - 当前 `mask` 已经极轻，不值得优先继续抠
- 先改 rerank / payload 后段
  - 现有 perf 仍显示 prepare/stage1 更值得优先处理

### 决策六：验收必须同时覆盖功能等价和热点迁移

后续实现的每一轮验证都必须同时回答两件事：

1. 结果是否与参考路径等价
2. 热点是否真的从目标段迁移出去

因此验收口径固定为：

- full E2E：recall、avg/p50/p95/p99、prepare/stage1 细分字段
- query-only perf：CPU 热点占比排序

## Risks / Trade-offs

- [融合版实现复杂度高] → 先保留两段参考路径，逐 query 校验等价，再切默认路径。
- [LUT layout 改写易引入隐蔽错误] → 将 group LUT、byte plane、packed write 三段单独建边界，避免一次性混写。
- [Stage1 SIMD 化后收益被 submit 或 address decode 吞掉] → 先记录完整候选清单与优先顺序，避免错误投资在 `mask` 这类轻热点上。
- [prepare 细分字段过多带来 benchmark 污染] → 继续保留 `fine_grained_timing` 区分解释模式与 clean perf。
- [融合版收益可能低于预期] → 同时记录“保留两段版 LUT SIMD 化”作为备选实现路线。

## Migration Plan

1. 先落本 change，作为 `quantize + lut_build` 融合与下一轮 SIMD 的总设计入口。
2. 后续实现优先顺序固定为：
   - 融合路径设计与参考路径等价校验
   - `BuildFastScanLUT` 内部三段化
   - Stage1 survivor compaction / batch classify
3. 每完成一轮实现，必须回填 full E2E 与 query-only perf 结果。
4. 若融合版收益不足，再回退到“两段版 + LUT SIMD 化”的保守路线。

## Open Questions

- 融合版是否需要在 AVX-512 与 AVX2 下分别维护两套 packed write 路径，还是先在 AVX-512 下主推。
- `BuildLut16` 是否应被保留为 scalar reference helper，供 debug / 对拍使用。
- Stage1 survivor compaction 是否需要新增专门的 bitmask-to-index helper，还是复用现有 batch 结构逐步演进。
