## Context

当前 `probe_submit` 结构在 [overlap_scheduler.cpp](/home/zcq/VDB/VectorRetrival/src/query/overlap_scheduler.cpp) 中大致如下：

```text
ClusterProber::Probe(...)
  -> CandidateBatch
  -> AsyncIOSink::OnCandidates(batch)
     -> for each candidate:
        -> dedup insert
        -> CRC heap update
        -> AllocatePendingSlot
        -> buffer acquire
        -> PrepReadTagged / PrepReadRegisteredBufferTagged
        -> per-candidate timing
```

这条路径的问题不是算法错误，而是结构上仍然按“逐 candidate 热路径”组织，导致批处理信息没有真正被利用。最近 profiling 的结论已经比较明确：如果继续优化 `probe_submit`，最先值得做的是三项耦合改造：

1. batched submit skeleton
2. batch-aware dedup
3. CRC/timing off hotpath

## Goals / Non-Goals

**Goals:**

- 将 `probe_submit` 改造成真正的 batch-oriented 提交路径。
- 减少 dedup、slot、buffer、`PrepRead*` 和计时相关的 per-candidate 常数项。
- 保持现有结果语义不变，并维持现有 `probe_submit_ms` / `uring_prep_ms` / candidate 计数字段可比较。
- 让后续如果需要继续往 `AsyncReader` 增加更轻的批量 prep helper，也能在当前结构上自然扩展。

**Non-Goals:**

- 不改 Stage 1 / Stage 2 分类逻辑。
- 不改 rerank 和 payload 路径语义。
- 不在本轮引入新的外部 hash 容器依赖。
- 不把 CRC 更新推迟到 all-probe-done 之后。

## Decisions

### 决策一：先在 `AsyncIOSink` 内部建立 batched submit skeleton

第一阶段不直接重写 reader 接口，而是在 `AsyncIOSink` 内部引入固定容量 batch scratch，把 `OnCandidates(batch)` 拆成三个内部阶段：

```text
OnCandidates(batch)
  -> ScanAndPartitionBatch(...)
  -> ReserveSlotsAndBuffers(...)
  -> PrepBatchReads(...)
```

其中：

- `ScanAndPartitionBatch` 负责扫描 `CandidateBatch`、生成 unique candidate 列表、按 `safein_all / vec_only` 分桶。
- `ReserveSlotsAndBuffers` 负责批量准备 slot 和 buffer。
- `PrepBatchReads` 负责按分桶顺序批量调用 `PrepRead*`。

这样做的原因是，第一步最大的收益来自“先把逐条路径拆成 batch 路径”，即使暂时还沿用当前 dedup 结构和 reader API，也能先拿到主要结构收益。

替代方案是直接向 `AsyncReader` 增加批量 prep API，但这会把范围一下推到 reader 层，实现风险更大。

### 决策二：batch-aware dedup 分两层完成

第二阶段 dedup 分两层：

1. batch-local dedup
2. query-global dedup

具体策略：

- 对 `CandidateBatch` 中最多 32 个 offset，先做 batch-local 去重。
- 再对 batch-local unique 项执行 query 级 dedup。
- query 级 dedup 不再使用 `std::unordered_set<uint64_t>` 作为最终方案，而是改为 query-local 的轻量开放地址表或等价平铺结构。

这样做的原因是，当前 batch 很小，先局部去重能明显减少打全局 dedup 表的次数；而 query 生命周期内的 dedup 表只需要 `insert + clear`，不需要通用容器的全部能力。

### 决策三：CRC heap 更新只下沉到 cluster-end merge

CRC 相关的 `est_heap_` 更新不能推迟到所有 cluster probe 完成之后，否则会破坏 early stop 的 cluster 粒度语义。当前折中方案是：

- submit 时只收集 cluster-local estimate
- 当前 cluster probe 结束后再统一 merge 进 `sched_.est_heap_`
- `ShouldStop()` 仍然在 cluster 边界判断

这样既能把 per-candidate heap 更新从 submit 热路径拿走，又不改变 early stop 行为边界。

### 决策四：细粒度计时从 per-candidate 收敛到 batch/group 级

当前 `SubmitOne()` 内频繁调用 `steady_clock::now()`。第一轮改造后，计时边界改成：

- 整个 batch submit wall time
- `safein_all` 分组 prep time
- `vec_only` 分组 prep time
- `dedup_and_slot_ms = batch_submit_ms - prep_read_ms`

这保持了与现有 `probe_submit_ms` / `uring_prep_ms` 的语义兼容，同时减小 measurement overhead。

### 决策五：结果语义必须保持不变

本 change 的所有实现都受以下约束：

- SafeIn / Uncertain 分类语义不变
- `VEC_ALL` / `VEC_ONLY` 选择语义不变
- rerank / payload 路径不变
- recall 与最终排序语义不变

也就是说，这轮只能改提交流程，不能借机混入其它算法或语义改动。

## Risks / Trade-offs

- [batched submit 增加代码复杂度] → 通过固定容量 scratch 和阶段化 helper 控制复杂度，不引入动态结构泛滥。
- [query-global dedup 自定义结构容易出错] → 先保持语义与当前 `unordered_set` 一致，再逐步替换，必要时保留对照路径。
- [CRC merge 改动影响 early stop] → 明确只能在 cluster-end merge，并要求 benchmark 回归校验 early stop 行为。
- [计时边界变化影响结果对比] → 保持字段名不变，并在 spec 中固定新边界。

## Migration Plan

1. 先实现 batched submit skeleton，暂时保留现有 dedup 容器与 CRC 逐 candidate 逻辑。
2. 在新 submit 骨架上接入 batch-local dedup 和 query-global 轻量 dedup 表。
3. 再将 CRC estimate 收集改成 cluster-end merge，并把计时收敛到 batch/group 级。
4. 每一步都使用同参数 full E2E + query-only perf 复核收益与热点迁移。

## Open Questions

- query-global dedup 第一版是否直接自定义开放地址表，还是先引入中间层以便保留 `unordered_set` fallback。
- batched submit skeleton 是否足以满足当前收益目标，还是需要在下一轮继续向 `AsyncReader` 层引入批量 prep helper。
