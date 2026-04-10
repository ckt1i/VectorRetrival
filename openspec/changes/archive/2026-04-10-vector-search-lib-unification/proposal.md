## Why

过去三轮优化（`encoder-mbit-fast-quantize` + `eps-calibration-simd` + `calibration-omp-parallel`）把 Deep1M Phase B 从 **90.5 s 压到 ~20.6 s**（~4.6×）。但一次 grep 揭示了一个扎心的事实：

```bash
$ grep -r "FastScanSafeOutMask\|SignedDotFromBits\|rotated_centroids\|
          PrepareQueryRotatedInto\|PrepareQueryInto\|eps_ip_fs"  src/index/ src/query/
(no matches)
```

**所有这些优化都只存在于 `benchmarks/bench_vector_search.cpp` 里**。`src/index/ivf_builder.cpp` 和 `src/query/overlap_scheduler.cpp` 仍在跑三轮优化之前的旧代码。

这意味着：
1. **`bench_e2e` 没有获得任何一项最近的加速**——它通过 `IvfBuilder` + `OverlapScheduler` 走库路径，而库路径还是旧逻辑。
2. **向量侧的每次创新都要在 bench_vector_search 里重做一次**——因为两条路径（内存 inline vs 库 API）的数据结构和热循环是分叉的。
3. **库里还留着 popcount 路径**，但 `query/overlap_scheduler.cpp` 已经只走 FastScan，popcount 实际上已经死代码。

### 代码分叉示意

```
bench_vector_search.cpp (1261 行, 全 inline)          library (src/index + src/query)
══════════════════════════════════════════            ═════════════════════════════════

main() {                                              IvfBuilder::Build()
  KMeans                                              ├─ eps_ip 校准（旧 popcount 公式）
  Encode                                              ├─ 不算 rotated_centroids
  eps_ip + SignedDotFromBits + OMP          ★          └─ 写 .clu v7（无 rotation）
  FastScan pack                                       
  d_k                                                 IvfIndex::Open()
  eps_ip_fs + 截断 + OMP                   ★          └─ 读 rotation.bin 和 centroids.bin
  CRC (OMP)                                           
                                                      OverlapScheduler::ProbeCluster()
  main search loop:                                   ├─ scalar per-lane Classify 循环   ❌
    rotated_centroids 预计算              ★           ├─ PrepareQuery 每 cluster 重做    ❌
    PrepareQueryRotatedInto               ★           ├─ 没用 FastScanSafeOutMask        ❌
    FastScanSafeOutMask batch classify    ★           └─ 没用 rotated_centroids          ❌
    ctz 跳过 SafeOut lanes                ★
    Stage 2 + _mm_prefetch                ★
    inline L2Sqr rerank (纯内存)
}                                                     bench_e2e.cpp 用这条路径
                                                      → 吃不到上面任何 ★ 的收益
```

### 额外的两个结构性问题

1. **bench_vector_search 完全在内存中跑**：load 完 `base.fvecs` 就把所有数据留在内存，搜索时用指针直接访问。这和未来「从 .clu 分批读取 + I/O 流水协同」的实验方向南辕北辙——我们需要 bench_vector_search 也走磁盘路径，才能在它上面做创新实验，再无缝迁移到 bench_e2e。

2. **bench_e2e 没法跳过 build**：每次运行都要 KMeans + Encode + calibrate，即使你只想换个 query 参数测延迟。需要一个 `--index-dir` 参数直接加载已有的 .clu/.data。

## What

分 4 个阶段推进（每阶段独立验证，可独立回滚）：

### Phase 1 — 库的向量侧算法升级（无格式改动）

把 bench_vector_search 里的向量侧优化搬入库，**但不改 .clu 格式**：
- `IvfBuilder` 的 eps 校准改用 `eps_ip_fs` 公式（FastScan distance error 归一化 + d_k 截断）
  - 内部用 `SignedDotFromBits` + OMP parallel（从 bench 搬过来）
  - 保留字段名 `calibrated_eps_ip_`，但语义改为 FastScan 版
- `IvfBuilder` 在 build 时预计算 `rotated_centroids`（Hadamard 路径）
  - 暂时放在 `IvfIndex::Open` 之后即时计算（因为格式还没升版）
- 删除库里的 popcount 相关死代码（纯向后兼容的部分）
- 删除 bench_vector_search 的 popcount 对照路径（放弃向后兼容）

### Phase 2 — .clu v8 格式升级

一次性整理磁盘格式，让 IvfIndex 能从磁盘直接拿到所有需要的东西：
- .clu v8 集成 `rotation matrix`（原 `rotation.bin` 不再独立存在）
- .clu v8 携带 `rotated_centroids`（build 时一次性写入）
- `segment.meta` 里写入 `eps_ip_fs` 和 `d_k`（目前 `eps_ip` 在 segment.meta 里）
- **不保留 v7 读取能力**（user 确认：开发阶段重建即可）
- `IvfIndex::Open` 读取 v8 并填充内存字段

### Phase 3 — ClusterProber 抽取

把**向量侧分类热循环**抽成一个可复用的单元，两条 rerank 路径都用它：
- 新增 `vdb::index::ClusterProber`：`Probe(ParsedCluster, PreparedQuery, dynamic_d_k, Sink&)`
  - 内部：`FastScanSafeOutMask` + ctz 迭代 + Stage 1 ClassifyAdaptive + Stage 2 IPExRaBitQ
- 新增 `vdb::index::ProbeResultSink` 接口：`OnSafeIn` / `OnUncertain`
- `OverlapScheduler::ProbeCluster` 变薄：委托 `ClusterProber` + `AsyncIOSink`（内部封装原有 `PrepRead`）
- 数值完全 bit-exact，验证 `bench_e2e` 结果不变只是快了

### Phase 4 — bench 统一到磁盘路径 + 纯查询模式

- **bench_vector_search 重写**：删掉 ~700 行 inline 代码，改用 `IvfBuilder::Build()` 写 .clu/.data，然后用 `OverlapScheduler` 从磁盘分批读取，和 bench_e2e 的主体结构一致
- **bench_vector_search 和 bench_e2e 共享 `--index-dir` 参数**：当指定时，跳过 build，直接 `IvfIndex::Open()` 加载既有索引，进入纯查询测试模式
- 新增轻量 helper（可放 `benchmarks/common/` 或直接 copy-paste）负责 "build-or-load" 决策

## Expected Impact

**bench_e2e（主实验，核心目标）**：
- SafeOut 比例上升 → I/O 提交量下降 → 带宽压力下降
- 每 cluster 省掉一次 FWHT（Hadamard 路径）→ probe CPU 时间下降
- 预期 `avg_cpu` 和 `avg_io_wait` 都下降，`overlap_ratio` 上升
- `recall@10` 不变，数值 bit-exact（Phase 3 的验证目标）

**bench_vector_search**：
- 数值和目前一致（Phase 4 验证目标）
- 从纯内存转为磁盘路径后，**首次可以在 bench_vector_search 上测 I/O 协同优化的创新点**，然后无缝迁到 bench_e2e

**代码健康度**：
- bench_vector_search 从 1261 行缩到 ~400 行
- 向量侧的未来优化只需改 `ClusterProber`，两条 bench 自动受益
- 删除 ~500 行 popcount 死代码

## Non-Goals

- 不做 per-block-32 粒度的 CPU-I/O 流水优化（未来的 `probe-io-block32-interleaved`，依赖本 change 先就位）
- 不改 RaBitQ encoder 本身（上一轮 `encoder-mbit-fast-quantize` 已处理）
- 不改 CRC 校准的数学逻辑
- 不保留 .clu v7 的读取能力（开发阶段重建）
- 不保留 bench_vector_search 的 popcount 对照路径
