## Why

当前 `redundant_top2` 的端到端实验已经证明，多归属能够明显缓解高召回区间对大 `nprobe` 的依赖；但现有实现仍采用“第二近 centroid”作为 secondary list 选择规则，这本质上仍是 naive redundant assignment。RAIRS 的结果和代码都表明，真正有效的改进点不只是“多放一个 list”，而是用更合理的残差感知指标选择第二个 list，因此现在需要先吸收 RAIR，而不是继续推进更重的 SEIL/list-layout 改造。

## What Changes

- 在 IVF builder 中新增 `RAIR` secondary-assignment 策略，用 AIR loss 替代当前“第二近 centroid”规则来选择 `top-2` 的第二个 cluster。
- 保留现有 `single` 和 `naive redundant_top2` 路径，支持三种 assignment mode 的并行对比：
  - `single`
  - `redundant_top2_naive`
  - `redundant_top2_rair`
- 将 RAIR 的核心参数显式暴露到构建配置和 benchmark CLI：
  - strategy / mode
  - `lambda`
  - strict-second-choice policy
- 继续复用当前已经落地的 `.clu V9 + raw address table` 存储格式；不引入 SEIL，不做 shared-cell block layout 优化，duplicated postings 直接存两份 cluster-side posting。
- benchmark 增加固定协议，对比 `single / naive top-2 / RAIR top-2` 在同一 `nlist / epsilon / alpha` 下的：
  - recall-latency
  - `avg_total_probed`
  - duplicate / dedup 统计
  - preload / build 成本
- 增加面向 coarse-partition 的诊断输出，明确回答 RAIR 是否进一步降低达到高 recall 所需的 `nprobe`。

## Capabilities

### New Capabilities
- `rair-secondary-assignment`: 支持基于 AIR loss 的 RAIR secondary list 选择，并允许与 naive top-2 redundant assignment 做直接对比。

### Modified Capabilities
- `e2e-benchmark`: 基准测试需要支持 `single / redundant_top2_naive / redundant_top2_rair` 三种 assignment mode 的统一构建、执行和导出。

## Impact

- Affected code:
  - `include/vdb/index/ivf_builder.h`
  - `src/index/ivf_builder.cpp`
  - `include/vdb/index/ivf_metadata.h`
  - `src/index/ivf_index.cpp`
  - `benchmarks/bench_e2e.cpp`
  - `tests/index/ivf_builder_test.cpp`
  - `tests/query/overlap_scheduler_test.cpp`
- Affected behavior:
  - redundant top-2 的 secondary cluster 不再固定等于第二近 centroid
  - assignment mode 成为显式的构建与 benchmark 维度
  - E2E 输出将新增 RAIR vs naive 的直接对照
- Non-goals:
  - 不引入 SEIL/shared-cell block layout
  - 不修改 query 扫描内核或 block-level dedup 机制
  - 不再次更改 `.clu` 地址格式
