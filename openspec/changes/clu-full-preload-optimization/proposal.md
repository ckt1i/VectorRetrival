## Why

当前 `.clu` 读取路径采用滑动窗口式的批量读取，与查询阶段交错执行。同步 baseline 调参后，结果已经表明 BoundFetch 明显优于 IVF+PQ+FlatStor，但仍落后于 tuned DiskANN+FlatStor；继续做通用 submit-path 微调的边际收益已经不高。下一步更值得验证的最小结构变化，是在查询开始前一次性读取整个 `.clu` 文件中的量化向量和地址相关元数据，移除 cluster-side I/O 与解析在热路径上的反复开销，并用同一套 warm benchmark 直接比较修改前后的查询速度提升。

## What Changes

- 将现有 `.clu` 读取策略从“滑动窗口式按需批量预取”改为“查询前一次性全量读取并常驻内存”。
- 修改 cluster 读取与 probe 路径，使查询阶段优先复用全量驻留的 `.clu` 数据，而不是继续围绕 `.clu` 发起增量 I/O。
- 为全量 preload 路径增加明确的生命周期和内存约束：
  - 在 index open 或 benchmark warmup 阶段完成加载
  - 查询阶段不得重复读取 `.clu`
  - 仍然保持 payload / 原始向量读取路径不变
- 为 benchmark 增加“修改前 vs 修改后”的对比要求，至少报告：
  - 相同参数下的 `recall@10`
  - `e2e_ms`
  - `p99_ms`
  - 查询阶段与 `.clu` 相关的耗时变化
  - preload 时间与额外驻留内存
- 明确比较目标是查询速度提升，而不是通过改变召回语义来换取更快结果。

## Capabilities

### New Capabilities
- `clu-full-preload`: 定义 `.clu` 文件在查询前一次性全量加载、驻留和查询期复用的行为约束

### Modified Capabilities
- `async-cluster-prefetch`: 将 cluster 侧读取主路径从滑动窗口预取调整为优先使用全量 preload，明确旧滑窗路径降级为 fallback 或兼容实现
- `e2e-benchmark`: 增加修改前后对照实验的输出要求，确保能量化查询阶段速度提升与 preload 成本

## Impact

- Affected code:
  - cluster store / `.clu` 读取路径
  - overlap / probe 查询热路径
  - benchmark result export and analysis path
- Affected systems:
  - warm steady-state query serving
  - benchmark warmup / startup behavior
- Affected artifacts:
  - `openspec/specs/async-cluster-prefetch/spec.md`
  - `openspec/specs/e2e-benchmark/spec.md`
  - new spec under `openspec/changes/clu-full-preload-optimization/specs/clu-full-preload/spec.md`
- Affected experiments:
  - before/after comparison on the same dataset and parameter setting
  - query-speed improvement measurement after full `.clu` preload
