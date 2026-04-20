## Context

当前 `FindNearestClusters()` 的 coarse 路径已经拆成了三个主要部分：

```text
query
  -> query normalize（仅 cosine）
  -> centroid score
  -> nth_element + sort(top-nprobe)
  -> cluster id materialize
```

在当前验收参数下，`coarse_topn_ms` 已经很小，而 `coarse_score_ms` 占了 coarse 阶段的主要成本。进一步检查代码可知：

- `effective_metric=l2` 路径已经有 `simd::L2Sqr` 或 MKL `sgemv` 支撑；
- `effective_metric=ip` 且 `requested_metric=cosine` 路径仍然是：
  - 先 normalize query；
  - 再对所有 centroid 做逐维标量点积；
  - 将 `-score` 写入 `scores[]`。

因此，这一轮 change 的目标非常收敛：只优化 coarse 的 centroid score kernel，并围绕这一 kernel 把合适的数据布局前移到 `Open()` 或查询前准备阶段。query normalize 和 top-n selection 本轮保持现状，不一起重写。

## Goals / Non-Goals

**Goals:**
- 用多 centroid 并行展开的 SIMD register-blocking kernel 替换 coarse `ip/cosine` 路径中的标量 centroid score 循环。
- 为 coarse score 增加 query 期零重排的 packed centroid layout，让 query 直接读取最适合 SIMD 的布局。
- 将 coarse packed layout 的构建前移到 `IvfIndex::Open()` 或等价的 query 前准备阶段，而不是在 query 开始后现场转换。
- 保持 query normalize 和 top-n selection 语义不变，确保本轮只观察 centroid score kernel 的收益。
- 保持 `coarse_select_ms`、`coarse_score_ms`、`coarse_topn_ms` 的统计边界稳定。

**Non-Goals:**
- 不在本轮改写 cosine query normalize 的执行位置或实现方式。
- 不在本轮重写 `nth_element + sort(top-nprobe)`，也不引入新的 top-k selection 算法。
- 不在本轮把 coarse score 主路径直接切到外部 BLAS/GEMV 依赖；GEMV 只作为后续可选 backend 和设计预留。
- 不修改索引文件格式，不要求把 packed layout 写回磁盘。

## Decisions

### 决策一：coarse score 主实现采用多 centroid 并行展开的 SIMD register-blocking

本轮 coarse score 的主实现选择为：

- 一次处理一个 centroid block，而不是一次只处理一个 centroid；
- query 的每个向量块只加载一次；
- 同时更新多个 centroid accumulator；
- 最终一次写回对应 block 的 `scores[]`。

逻辑上类似：

```text
for centroid_block in blocks(B):
    acc[0..B-1] = 0
    for dim_block in blocks(VL):
        qv = load(query[dim_block])
        for lane in 0..B-1:
            cv = load(packed_centroids[centroid_block][dim_block][lane])
            acc[lane] += qv * cv
    scores[block] = -horizontal_sum(acc)
```

这里的 `B` 是 register-blocking 的并行 centroid 数，取决于目标 ISA：

- AVX2：优先 `B=4` 或 `B=8`
- AVX-512：优先 `B=8` 或 `B=16`

这样做的原因是当前热点不是单次 dot 的数学正确性，而是“同一个 query 被重复喂给 2048 个 centroid”的批量打分模式。按多 centroid 展开可以显著减少 query 重复加载，并更好利用寄存器和 cache。

备选方案：
- 仅补一个 `simd::Dot(query, centroid)`，外层仍保持逐 centroid 循环。
  这个方案改动小，但仍然没有利用“这是一个 batched centroid scoring 问题”的结构，因此不作为本轮主方案。

### 决策二：SIMD 与 GEMV 不是互斥关系，而是不同层级的实现方式

这轮设计里明确区分两层概念：

- `SIMD`：底层向量化实现手段，是算子级或 micro-kernel 级能力；
- `GEMV`：矩阵乘向量的运算形态，是更高层的批处理接口。

两者不是互斥关系。更准确地说：

- 高性能 GEMV 实现通常本身就是建立在 SIMD micro-kernel、blocking、packing 之上的；
- 我们自己写的多 centroid register-blocking coarse kernel，本质上也可以看作“面向特定形态的轻量 GEMV special-case”。

因此，本轮的工程决策是：

1. **先实现 in-tree SIMD register-blocking kernel**，作为默认 coarse score 路径；
2. **同时保留 GEMV backend 的接入点**，后续若要引入 MKL/OpenBLAS 或自研 GEMV-style backend，可以在不改 query 语义的前提下切换；
3. benchmark 维度上，后续可比较：
   - 标量 dot
   - 单 centroid SIMD dot
   - 多 centroid register-blocking
   - 外部 GEMV backend

这样做的原因是你当前已经明确希望“先做 SIMD 优化”，而不是把这一轮扩大成“切 BLAS 依赖 + 改 query prepare + 改 top-n”的组合变更。先把 in-tree SIMD kernel 做扎实，既能直接落地，也不会阻碍后续 GEMV 化。

备选方案：
- 本轮直接把 `ip/cosine` 粗排主路径改成 GEMV。
  这条路可以走，但它会把本轮 change 带到库依赖、调用路径和 fallback 策略的更大讨论里，不利于先快速验证 register-blocking kernel 的收益，因此本轮不作为主交付。

### 决策三：packed centroid layout 在查询前准备完成，而不是 query 期间现场布局化

为了支撑多 centroid 并行 kernel，需要为 `centroids_` 和 `normalized_centroids_` 各自准备 coarse 专用 packed layout。这个布局不是简单的 row-major，而是面向 `B x VL` block 的 packed 形式。

建议布局如下：

```text
packed_centroids
  = [centroid_block][dim_block][lane_in_block][elem_in_vec]
```

线性化后可理解为：

```text
for cb in 0..ceil(nlist/B):
  for db in 0..ceil(dim/VL):
    store centroid(cb*B + 0)[db*VL : db*VL+VL]
    store centroid(cb*B + 1)[db*VL : db*VL+VL]
    ...
    store centroid(cb*B + B-1)[db*VL : db*VL+VL]
```

也就是把“同一个维度块上的多个 centroid”打包到一起，让 kernel 在一次 query 块加载之后连续消费整组 centroid 数据。对于尾块：

- `nlist` 不是 `B` 的整数倍时，尾部 centroid 用零填充或 masked load；
- `dim` 不是 `VL` 的整数倍时，尾部元素在 packed 时补零，避免 query 阶段专门做复杂尾处理。

这一步必须前移到 `IvfIndex::Open()` 或 resident prepare 阶段，原因有三点：

1. query 时再布局化，会把本轮想省下的 coarse 时间重新花在转换上；
2. `centroids_` 与 `normalized_centroids_` 都是 index 级静态数据，天然适合一次准备、多次复用；
3. packed layout 的额外内存是稳定可控的，远比 query 期间重复重排更合理。

因此，本轮明确要求：

- `centroids_` 原始 row-major 表示继续保留，用于兼容其他路径；
- 新增 coarse packed view / buffer；
- `normalized_centroids_` 若存在，也同步准备自己的 packed view；
- query 期间只读 packed view，不做临时 pack。

备选方案：
- query 开始时对当前需要的 centroid 子集做临时 pack。
  这个方案理论上能减少常驻内存，但会把布局化成本重新放回 query 主路径，不符合这轮优化目标，因此不采用。

### 决策四：query normalize 与 top-n selection 本轮保持不动，作为隔离变量

虽然 query normalize 和 top-n selection 后续仍可优化，但这轮明确不动它们，原因是：

- 你已经希望先把 centroid score 的收益单独测干净；
- 目前 coarse 的主瓶颈已经明确在 score kernel，而不是 top-n；
- 如果把 normalize、score、selection 一起动，之后很难归因到底是谁带来了收益。

因此，本轮 coarse 优化的验收边界固定为：

- `coarse_score_ms` 下降是主目标；
- `coarse_topn_ms` 只要求语义不变、统计不丢；
- query normalize 仍然沿用当前逻辑。

## Risks / Trade-offs

- [风险] packed centroid layout 会增加 index 常驻内存。  
  → Mitigation：仅为 coarse 相关路径保存一份 packed buffer；继续保留原始 row-major 以保证兼容，但不为其它非 coarse 路径额外复制更多副本。

- [风险] register-blocking kernel 的最优 block size 强依赖 ISA 与 dim。  
  → Mitigation：按 AVX2 / AVX-512 分开提供默认 block 配置，并保留统一 fallback 路径，先追求稳定收益，再做自动调参。

- [风险] 手写 SIMD kernel 复杂度上升，可能引入 correctness 回归。  
  → Mitigation：要求新 kernel 对标量 coarse score 结果保持数值等价容忍范围，并保留 benchmark 前后的 recall 对比。

- [风险] 若后续再引入 GEMV backend，可能出现两套 coarse 实现长期并存。  
  → Mitigation：从设计上把 coarse score 抽象成统一入口，当前 SIMD kernel 和未来 GEMV backend 都挂在同一 coarse score dispatch 下。

## Migration Plan

1. 在 OpenSpec 中落地 coarse centroid score kernel、query-pipeline 和 e2e-benchmark 的变更要求。
2. 在 `IvfIndex` 中增加 coarse packed layout 的持有结构，并在 `Open()` 时构建 `centroids_` 与 `normalized_centroids_` 的 packed 视图。
3. 实现 `effective_metric=ip` 路径下的 SIMD register-blocking centroid score kernel，并通过统一入口接入 `FindNearestClusters()`。
4. 保留现有 query normalize 与 top-n selection，确保本轮收益集中反映在 `coarse_score_ms`。
5. 编译通过后，以同参数 benchmark 和 perf 复测，确认 coarse score 热点下降并记录是否需要下一轮引入 GEMV backend。

## Open Questions

- AVX2 与 AVX-512 下的默认 centroid block 大小是否需要分别做小规模 autotune，还是先固定经验值即可。
- coarse packed layout 是否需要 64B 对齐分配和 page-aligned 分段，以进一步提升流式读取效果。
- 在后续引入 GEMV backend 时，是否复用这次 packed layout，还是为 GEMV 维持一份更接近 BLAS 期望的 row-major/column-major 视图。
