## Context

现有 ExRaBitQ Stage2 已完成第一阶段 kernel 重写，查询侧已从“重建 signed float(code)”切换到“signed-query dot abs(code) + bias”路径，但 resident + IVF 的 clean perf 仍显示 `IPExRaBitQ` 是主路径上的显著 CPU 热点。进一步分析当前 `.clu` Region2 布局后，问题已经从单纯算子实现收敛为存储格式本身：

- 当前每条 ExRaBitQ entry 采用 AoS：
  - `ex_code[dim bytes]`
  - `ex_sign[dim bytes]`
  - `xipnorm[4 bytes]`
- `ex_sign` 本质只承载 1 bit 语义，却按 `1 byte / dim` 存储
- resident 查询期仍需为每个 candidate 从 entry 中读取整段 sign bytes，再在线生成 SIMD lane sign mask
- `.clu` 读取器、resident preload、`ParsedCluster` 和 `IPExRaBitQ` 仍默认 sign 是逐维 byte，因此格式冗余已经渗透到了构建、加载和查询全链路

本 change 只处理“存储格式升级”这条线，不回头混入第一阶段 kernel 话题。目标是先把 `sign bit-pack` 作为一版稳定的新格式落地，并把后续 `compact ex-code layout` 预留为下一阶段扩展，而不是在一个 change 里同时重做两类风险不同的结构。

## Goals / Non-Goals

**Goals:**

- 为 ExRaBitQ Region2 定义一个版本化的新存储格式，使 `ex_sign` 从逐维 byte 升级为 packed sign bits。
- 明确新格式在编码、`.clu` 写入、读取、resident preload、`ParsedCluster` 视图和 `IPExRaBitQ` kernel 中的统一边界。
- 让 query path 在新格式下直接消费 packed-sign，而不是继续把逐维 byte sign 作为持久化主表示。
- 为 `.clu` 文件版本升级提供明确的 rebuild、兼容、回滚和 benchmark 观察策略。
- 为后续 `compact ex-code layout` 保留扩展空间，但不把它作为本 change 的首个交付目标。

**Non-Goals:**

- 本 change 不改变 ExRaBitQ 的数学语义、recall 定义、排序语义、CRC 逻辑或 payload 语义。
- 本 change 不要求立即实现跨 candidate 的 block-batched Stage2 kernel。
- 本 change 不要求一次性引入完整的 cluster-level SoA / block-interleaved Region2 紧凑布局。
- 本 change 不修改 Stage1 FastScan、coarse select 或 rerank 路径。

## Decisions

### 决策一：先做 `sign bit-pack`，不在本 change 内直接切到完整 compact layout

本 change SHALL 先把 ExRaBitQ entry 从：

```text
[ex_code: dim bytes][ex_sign: dim bytes][xipnorm: 4]
```

升级为：

```text
[ex_code: dim bytes][packed_sign: ceil(dim/8) bytes][xipnorm: 4]
```

并保持“每个向量一条 entry”的 AoS 组织不变。

原因：

- 这是当前收益 / 风险比最高的一步，能先压掉最明显的 sign 带宽冗余。
- 构建器、writer、reader、resident parse 和 kernel 都可局部修改，不需要立刻重做整个 Region2 组织。
- 该步骤能够为 query kernel 提供 `__mmask16` / `__mmask32` 友好的 packed sign 来源。

备选方案：

- 直接把 Region2 改成 cluster-level compact SoA / block-interleaved 布局。
  长期上限更高，但一上来会把风险扩散到构建、解析、回滚和调试链路，不适合作为第一刀。

### 决策二：通过新的 `.clu` file version 显式区分旧格式和 packed-sign 新格式

新格式 SHALL 使用新的 `.clu` 文件版本，而不是尝试通过 `bits`、entry 大小或启发式逻辑隐式推断布局。

原因：

- 旧格式 `v9` 中 `exrabitq_entry_size = 2*dim + 4`，新格式 entry 大小将变为 `dim + ceil(dim/8) + 4`，继续复用旧版本容易导致 reader 静默误解析。
- 显式版本化可以把兼容策略写清楚：旧 reader 拒绝新文件，新 reader 可按版本读取旧/新布局。
- benchmark 和实验结果也需要暴露实际使用的 storage version，便于解释收益来源。

备选方案：

- 不升版本，只通过 lookup / trailer 元数据推断。
  该方案调试成本高，出错时很难定位，且不利于回滚。

### 决策三：编码层只保留 packed sign 作为持久化主表示，逐维 `ex_sign` 降级为离线临时态

`RaBitQCode` 的持久化语义 SHALL 从“`ex_code + ex_sign`”切换为“`ex_code + packed_sign`”。如果实现阶段需要保留逐维 sign 作为离线编码临时缓冲或 debug 对拍手段，该表示 MUST 不再作为 cluster store 的主写入格式。

原因：

- 如果 `ex_sign` 继续作为主字段保留，writer / reader / kernel 很容易继续依赖旧语义，导致新格式只是“旁路优化”而不是主路径升级。
- 将 packed sign 提升为主表示，才能迫使解析接口和 kernel 一次性对齐。

备选方案：

- 同时长期保留 `ex_sign` 和 `packed_sign` 两套持久化表示。
  该方案会直接浪费空间，并长期增加维护负担。

### 决策四：`ParsedCluster` 和 resident view 必须暴露 packed-sign 访问，而不是在解析时反解成逐维 byte

读取器和 resident preload SHALL 将 packed sign 以零拷贝或等价轻量视图形式传递给查询路径。解析阶段 MUST NOT 为了兼容旧 kernel 而把 packed sign 反解成逐维 byte sign buffer。

原因：

- 若 parse 阶段重新 materialize `dim bytes` sign，存储升级的主要收益会在 resident preload 时被抵消。
- packed-sign 直接暴露给 query kernel，才能把带宽收益真实传导到 clean perf。

备选方案：

- 读取时反解 packed sign，查询仍吃 byte-sign。
  这会把收益从 query 期转移到 preload 期，最终很可能净收益有限。

### 决策五：Stage2 kernel 的 packed-sign 支持以跨维度通用 block 为主，不对单一维度写死

`IPExRaBitQ` 的 packed-sign kernel SHALL 继续沿用跨维度 block 路线，例如 `64/32/16/tail`，并把 packed sign 解码组织为每个 block 对应的 lane mask，而不是只为 `dim=512` 写死。

原因：

- 后续会在不同数据集和维度上重复测试。
- 这一路线与第一阶段 kernel 重写保持一致，也为未来 `compact ex-code layout` 留下相同 block 边界。

备选方案：

- 为 512 维单独写最强特化。
  可以作为后续附加优化，但不应成为本 change 的主设计。

### 决策六：为下一阶段预留 `compact ex-code layout`，但本 change 只定义决策门，不强制一并落地

本 change SHALL 在 design 与 spec 中明确说明：若 `sign bit-pack` 落地后，Stage2 仍是显著热点，则下一阶段可以把 Region2 升级为 cluster-local compact layout，例如 block-interleaved 组织。但该结构不作为本 change 的 mandatory deliverable。

原因：

- 这样可以把“先压 sign 带宽”和“重做 Region2 组织”分成两个清晰决策点。
- 避免一次性把风险拉满，也便于阶段性 benchmark / perf 判断是否值得继续。

## Risks / Trade-offs

- [新旧 `.clu` 格式并存增加读取复杂度] → 用显式 file version 和集中式 `exrabitq_entry_size(version)` / parse path 来隔离分支，而不是在 query 路径零散判断。
- [packed-sign kernel 复杂度上升] → 保留 reference/scalar 对拍路径，并用 query-only clean perf 和 full E2E 同时验收。
- [解析期若错误地 materialize byte-sign 会吃掉收益] → 在设计和 spec 中明确禁止将 packed sign 反解为 resident 主表示。
- [格式升级要求 rebuild，影响已有索引可用性] → benchmark / CLI / 输出中显式暴露 storage version，并为旧文件定义清晰的接受 / 拒绝 / 重建策略。
- [仅做 sign bit-pack 后收益不足] → 在 change 中预留 compact layout 决策门，避免把当前 change 绑定成“一次做到底”。

## Migration Plan

1. 定义新的 `.clu` file version，并把 ExRaBitQ Region2 的 packed-sign entry layout 固化到 spec。
2. 更新编码输出与 cluster writer，使新构建索引默认写出 packed-sign 格式。
3. 更新 reader / resident preload / `ParsedCluster` 视图，使 query path 可直接访问 packed sign。
4. 更新 `IPExRaBitQ` packed-sign kernel，并保持旧格式读取路径可作为兼容或回归对照。
5. 用 query-only clean perf 与 full E2E 对比旧格式和新格式，确认 recall 不退化、Stage2 观察值下降。
6. 若验证通过，再决定是否开启下一轮 `compact ex-code layout` change。

回滚策略：

- 代码级回滚：保留旧版本 parse / kernel 路径，必要时仅停用新 file version 的构建输出。
- 数据级回滚：对已生成的新格式 `.clu` 不承诺旧 reader 兼容；若需回退到旧 serving 二进制，必须使用旧格式索引或重新 build。

## Open Questions

- packed sign 的 bit ordering 是否统一采用“低位对应低维度”的 little-endian bit order，还是与现有 FastScan packed sign 共用同一辅助定义。
- 新 file version 是否只升级 ExRaBitQ Region2，还是顺带把相关元数据字段（如 storage version 输出）一并制度化。
- 若下一阶段进入 `compact ex-code layout`，应更偏向 RaBitQ-Library 风格的 cluster-local compact layout，还是优先保留当前 cluster block 三段式大框架。
