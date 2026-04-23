## Context

最近一轮 clean perf 已经把 query 主路径的热点重新排序清楚了：`prepare_lut_build` 与 Stage1 `iterate/classify` 在前几轮优化后已明显下降，而 Stage2 的 `IPExRaBitQ` 已经成为新的纯计算主热点。与此同时，`AVX-512 + resident + IVF` 是当前可稳定依赖的 serving 约束，但后续仍会在不同数据集、不同维度上测试，因此新方案不能只针对 `dim=512` 写死特化路径。

当前 `IPExRaBitQ` 的主要问题不是“完全没 SIMD”，而是其数据流仍然偏 decode-heavy：

```text
code_abs[u8] + sign[u8]
    -> reconstruct signed float(code)
    -> dot(query, signed_code)
```

这条路径在 resident 模式下会把查询期 CPU 时间花在 `u8 -> i32 -> f32`、sign mask 构造和 signed-float 重建上，而不是集中在更紧凑的点积上。此次 change 的目标是先把 kernel 自身的上限摸清，再决定是否值得推进存储格式升级。

## Goals / Non-Goals

**Goals:**

- 设计 `IPExRaBitQ` 的两阶段优化方案，优先保证在 `AVX-512 + resident + IVF` 下的跨维度稳定收益。
- 第一阶段在不改索引格式的前提下，重写 `IPExRaBitQ` kernel，减少 query 期 signed-float 重建与 decode 成本。
- 第二阶段仅在第一阶段后 `IPExRaBitQ` 仍是主热点时，才推进 `sign bit-pack` 与 `compact ex-code layout` 的设计。
- 固定 full E2E 与 clean perf 的双口径验收，明确什么情况下算“kernel 真变快”。
- 明确哪些优化是 query-global 可以前移的，哪些只能保持 per-candidate 在线执行。

**Non-Goals:**

- 本 change 第一阶段不改 recall、排序、CRC、payload、resident serving 语义。
- 第一阶段不改 cluster store 持久化格式，不引入 rebuild 强依赖。
- 不把 `EstimateDistanceFastScan`、submit、payload 路径的后续优化混入本 change 的第一阶段实现目标。
- 不为单一维度（如 512）硬编码专用逻辑作为主实现路径。

## Decisions

### 决策一：采用“先 kernel、后 layout”的两阶段路线

第一阶段 SHALL 保持现有 `code_abs[dim bytes] + sign[dim bytes] + xipnorm` 布局不变，只重写 `IPExRaBitQ` kernel。第二阶段只有在第一阶段后 `IPExRaBitQ` 仍然显著占用 query CPU 时，才进入 `sign bit-pack` 与 `compact ex-code layout`。

这样做的原因是：

- resident + IVF 场景下，当前最缺的是 query CPU kernel 效率，而不是先验地重做索引格式；
- 不改格式能最快验证 `IPExRaBitQ` 是否真的是主要矛盾；
- 若直接进入 compact layout，风险会从“kernel 优化”升级成“编码/存储/加载全链路重构”。

备选方案：
- 直接重做 ex-code 紧凑布局。
  该方案长期上限可能更高，但当前收益与风险都不够可控，因此不作为第一刀。

### 决策二：第一阶段 kernel 采用“幅值 + 符号分离消费”的 query-centric 路线

第一阶段 `IPExRaBitQ` SHALL 从当前的 code-centric 路线：

```text
code_abs + sign
  -> reconstruct signed float(code)
  -> dot(query, signed_code)
```

切换到 query-centric 路线：

```text
query + sign -> signed-query contribution
signed-query dot abs(code)
+ bias term from sign-applied query
```

更具体地，后续实现应优先考虑以下等价变换：

```text
Σ q[i] * sgn[i] * (abs[i] + 0.5)
= Σ (signed_q[i] * abs[i]) + 0.5 * Σ signed_q[i]
```

这样做的原因是：

- `code_abs` 仍可按 `u8 -> i32 -> f32` 轻量扩展，但不再需要先重建完整 signed float(code)；
- `sign` 只作为 query-side sign apply 使用，更容易按 block 做 SIMD 处理；
- `+0.5` bias 项可与主积累项解耦，减少每 lane 的冗余运算。

备选方案：
- 继续维持“重建 signed float(code)”的结构，只在现有 helper 上抠指令级细节。
  该方案属于小修小补，命中不了当前 decode-heavy 的根因。

### 决策三：第一阶段 kernel 必须按“跨维度通用 block”组织，而不是维度特化

由于后续要测试不同数据集与不同维度，第一阶段 kernel SHALL 以 `AVX-512` 的 block 组织为核心，而不是为 `dim=512` 写死。

推荐组织方式：

```text
64-dim block
  -> 32-dim block
    -> 16-dim block
      -> tail
```

这样做的原因是：

- 常见 embedding 维度（96/128/256/384/512/768/1024/1536）都能自然落到这套分层；
- resident serving 场景下，统一 block 策略更利于 query scratch 复用；
- 可以为第二阶段 `sign bit-pack` 或 compact layout 保留一致的 block 边界。

备选方案：
- 只为 `dim=512` 做最强特化。
  该方案在单数据集上可能更激进，但不符合当前“跨维度稳健”的要求。

### 决策四：第二阶段优先级是 `sign bit-pack`，再到 `compact ex-code layout`

如果第一阶段之后 `IPExRaBitQ` 仍然是 clean perf 的显著热点，则第二阶段的优先顺序固定为：

1. `sign bit-pack`
2. `compact ex-code layout`

原因如下：

- 当前 `sign` 使用 `uint8 per dim`，本质只承载 1 bit 信息，带宽和 cache 占用都偏高；
- 先做 `sign bit-pack` 能以较小的数据格式改动换取较直接的带宽收益；
- `compact ex-code layout` 是更强的长期方案，但会牵动 encoding / storage / parsing / migration，因此应放在后面。

备选方案：
- 直接从 `compact ex-code layout` 开始。
  该方案长期潜力最高，但当前不适合作为第一优先级。

### 决策五：clean perf 与 full E2E 必须继续双口径并行

后续所有阶段的验收 MUST 同时覆盖：

- clean perf：`query-only + fine_grained_timing=0`
- full E2E：正式 benchmark 参数

并且 clean perf 的要求是：

- prepare 细粒度打点不得混入主路径
- 必须能直接观察 `IPExRaBitQ`、`EstimateDistanceFastScan`、`AsyncIOSink/PrepRead` 等热点排序

这样做的原因是：

- 当前已经证明，统计污染会直接把 `avg_query` 拉高约 `0.18~0.20 ms`
- 如果不维持 clean perf，后续很难判断 `IPExRaBitQ` 的 kernel 改动到底有没有真实收益

## Risks / Trade-offs

- [第一阶段 kernel 收益低于预期] → 先以不改格式的方式验证上限，再决定是否进入第二阶段 layout 升级。
- [query-centric 路线增加 kernel 复杂度] → 保留当前 reference 逻辑作为回归基线，逐轮用 perf 验证热点是否迁移。
- [跨维度通用实现比单维度特化保守] → 明确本 change 目标是跨数据集稳健收益，而不是单一维度极限。
- [第二阶段数据格式升级牵动面过大] → 固定为条件触发，不作为第一阶段前置依赖。
- [clean perf 再次被统计污染] → 明确 `fine_grained_timing=0` 的打点边界，避免重复引入 `chrono` 干扰。

## Migration Plan

1. 建立本 change，明确 `IPExRaBitQ` 的两阶段路线与决策门。
2. 第一阶段先实现不改格式的 AVX-512 query-centric kernel。
3. 跑 clean perf 与 full E2E，对比：
   - `IPExRaBitQ` 占比是否显著下降
   - `avg_query` 是否稳定下降
4. 若第一阶段后 `IPExRaBitQ` 仍是主热点，则进入第二阶段：
   - 先做 `sign bit-pack`
   - 再决定是否推进 `compact ex-code layout`
5. 若第二阶段进入格式升级，必须明确 rebuild / migration 方案后再实施。

## Open Questions

- 第一阶段 kernel 是否需要同时保留一条更易读的 scalar / reference 路径，供 perf 之外的正确性对拍使用。
- `sign bit-pack` 若作为第二阶段第一步，是否应与 `xipnorm` / `norm_oc` 的因子重组一起推进。
- 第二阶段若进入 compact layout，是否应直接对齐 RaBitQ-Library 的 compact ex-code 思路，还是保留更贴合当前 cluster store 的本地布局。
