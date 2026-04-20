## Context

当前 resident 查询路径已经具备 batch dedup、CRC cluster-local merge、resident thin path 和分段 benchmark 统计，但 `probe_submit_ms` 与 `uring_submit_ms` 仍然保持在较高水平。现有路径的主要问题有两类：

- 用户态准备仍然偏碎：`SafeIn` / `Uncertain` 候选虽然已经按 batch 进入 `AsyncIOSink`，但 `PrepReadVecOnly/All` 的地址计算、slot 绑定、buffer 组织和 SQE 发射仍然更接近逐候选处理。
- `io_uring_submit` 调用仍然过多：每个 query 约有十余次 submit，内核入口固定成本没有被 batch 化收益覆盖。

这次 change 只做 query 主路径中 submit 相关的两段优化，不改变 `SafeIn` / `Uncertain` 分类语义，不改变 CRC estimate 的 cluster 边界，也不改变 early-stop 的判定时机。

## Goals / Non-Goals

**Goals:**
- 将 `PrepReadVecOnly/All` 收敛成按类型分桶的 batch prepare 路径，降低 `probe_submit_ms`。
- 将 submit 时机改成按 cluster 边界驱动的 windowed batch submit，降低 `submit_calls` 与 `uring_submit_ms`。
- 保持 resident + single-assignment 主验收路径下的 recall、结果排序、CRC/early-stop 语义与当前实现一致。
- 为 benchmark 提供足够的细化统计，分离“prepare 收益”和“submit 次数收益”。

**Non-Goals:**
- 不修改 `SafeIn` / `Uncertain` 的分类公式或 `VEC_ONLY` / `ALL` 的选择语义。
- 不引入新的 CLI 开关或运行时 autotune；第一版阈值先采用固定窗口策略。
- 不修改 batch rerank、payload 语义或 `io_uring` reader API。
- 不在本轮重做 dedup 语义或 CRC calibration 逻辑。

## Decisions

### 1. submit 改成 cluster-end windowed batching，而不是逐 `CandidateBatch` 立即提交

查询路径在每个 probed cluster 结束时统一检查当前累计待提交请求数：

- 若累计待提交请求数 `<= threshold`，则直接一次性提交全部待发请求。
- 若累计待提交请求数 `> threshold`，则按固定粒度分批提交。

首版阈值采用固定值 `8` 或 `16`，并按你当前要求写入设计约束：

- 逻辑上以 “待提交请求数” 为判断对象，不以候选数或 cluster 数本身为判断对象。
- “小于阈值则一次性全部提交，大于阈值则按阈值粒度分批提交” 作为默认策略。

选择这个策略而不是无限累积 submit 的原因：

- 保持 prefetch overlap，不把请求长期滞留在用户态。
- 不改 cluster 结束时的控制边界，便于与 CRC / early-stop / probe 生命周期对齐。
- 让 submit 次数下降，但避免因为过度延迟导致 `prefetch_wait_ms` 反弹。

备选方案与取舍：

- 方案 A：每个 `CandidateBatch` 立即 submit。
  已证明 syscall 次数过多，收益不足。
- 方案 B：跨多个 cluster 无限累积到 ring 接近满再 submit。
  可能进一步压 syscall，但更容易损伤 overlap，并放大调试复杂度。
- 方案 C：当前采用的 cluster-end windowed batching。
  在收益、语义稳定性和实现风险之间更平衡。

### 2. `PrepReadVecOnly/All` 改成两段式 batch prepare

对每个 cluster 收到的候选，先按类型分桶：

- `vec_only`
- `vec_all`

然后采用两段式处理：

1. `CollectReadPlan`
   只做 query 期必要的轻量 request plan 生成，如地址、长度、slot/buffer 绑定目标、请求类型等。
2. `EmitBatchReads`
   按 `ReadPlan` 连续写入 SQE 并登记 pending 状态。

这样做的原因：

- 避免 `PrepRead*` 在循环内部反复进行类型判断、slot 查找和 SQE 细节写入。
- 让地址计算、slot 预留、pending 填写和 SQE 发射各自变成线性处理。
- 为后续 windowed submit 提供自然的“先准备、后发射、再提交”边界。

备选方案与取舍：

- 方案 A：保留现有 `PrepReadVecOnly/All` 接口，仅在外层把多个 batch 合并。
  外形变了，但用户态常数项不容易真正下降。
- 方案 B：完全引入新的 reader API。
  侵入过大，超出本轮范围。
- 方案 C：保留现有底层 reader 契约，在 `AsyncIOSink` 内部新增 batch prepare 层。
  能拿到主要收益，同时不扩大修改面。

### 3. 继续保留 `SafeIn` / `Uncertain` 的语义分流

即使 submit 改成 windowed batching，候选仍然保持：

- `SafeIn` 走 `ALL`
- `Uncertain` 走 `VEC_ONLY`

并且仍然沿用当前解码与 completion 处理语义。这样做是为了避免把这次 change 和 payload / rerank / candidate correctness 混在一起，使性能收益可以直接归因到 submit 路径本身。

### 4. benchmark 增加 submit window 与 batch prepare 观测项

现有 `probe_submit_ms` 和 `uring_submit_ms` 足够说明“总成本”，但不足以说明收益来自哪里。第一版 benchmark 需要新增或细化以下观测：

- `avg_submit_window_flushes`
- `avg_submit_window_requests`
- `avg_submit_window_tail_flushes`
- `avg_batch_prepare_vec_only_ms`
- `avg_batch_prepare_all_ms`
- `avg_batch_emit_ms`

其中：

- `batch_prepare_*` 归入 `probe_submit_ms` 的子项。
- `submit_window_*` 用于解释 `uring_submit_ms` 与 `submit_calls` 的变化。

## Risks / Trade-offs

- `[Submit 延迟过头]` → `submit_calls` 会下降，但 `prefetch_wait_ms` 可能反弹。缓解方式是首版严格使用 cluster-end 触发和 `8/16` 固定阈值，而不是无限积压。
- `[批量 prepare 破坏 SafeIn / Uncertain 语义]` → 通过按类型分桶且复用现有 `ALL` / `VEC_ONLY` 分支的底层发射逻辑来避免。
- `[统计口径混淆]` → 明确规定 `probe_submit_ms` 是用户态 prepare+emit CPU 成本，`uring_submit_ms` 仅表示 submit wall-time，不把两者混算。
- `[实现复杂度上升]` → 第一版不做 autotune，不新增 CLI，不重构 reader 接口，把修改面限制在 `AsyncIOSink` 和 benchmark 聚合层。

## Migration Plan

1. 先在 `AsyncIOSink` 内部引入 request plan / batch emit 骨架，但默认仍可走现有单批提交方式。
2. 再切换为 cluster-end windowed submit，并用固定阈值 `8` 或 `16` 执行提交。
3. 为 benchmark 接入新增统计字段，确保能区分 prepare 收益和 submit 次数收益。
4. 使用固定 2000-query resident 参数回归，确认 recall 不变、`submit_calls` 和 `uring_submit_ms` 回落。

回滚策略：

- 若 windowed submit 导致 `prefetch_wait_ms` 明显升高，可仅保留 batch prepare，回退 submit 时机。
- 若 batch prepare 引入语义问题，可回退到现有 `PrepRead*` 逐批路径，同时保留 benchmark 新统计。

## Open Questions

- 首版默认阈值最终固定为 `8`、`16`，还是实现时保留编译期常量便于快速切换？
- `submit_window` 的粒度是否仅以“请求数”衡量，还是还需要受 ring 剩余 SQE 容量约束提前 flush？
- benchmark 是否还需要额外导出“每次 submit 平均 SQE 数”，用于更直接解释 `uring_submit_ms`？
