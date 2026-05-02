## Why

现有 query 主路径已经完成 `prepare/stage1`、coarse score SIMD、resident thin path 等一轮优化，但最新 profiling 表明，端到端时延仍由多个次级热点共同构成。现在需要先把这些剩余优化点以统一 change 的形式记录下来，明确优先级、预估收益和后续细化入口，避免后续分析与执行分散在多个临时讨论里。

## What Changes

- 新增一份 query 后续优化 backlog，集中记录 `probe_submit`、`coarse_select` 后续收敛、`probe_stage2`、prefetch/uring 链路、rerank 后段等候选点。
- 为每个优化点记录当前热点归因、适用前提、预估收益区间和推荐执行顺序。
- 将 `probe_submit` 的三项第一优先级工作明确写入 backlog：
  - `OnCandidates(batch)` 改成真正的 batch submit，而不是逐 candidate `SubmitOne()`
  - dedup 从 `unordered_set + per-candidate insert` 收敛为 batch-aware 去重路径
  - CRC heap 更新和 per-candidate 微计时从 submit 热路径中拆出，改为 batch / cluster 粒度
- 定义后续逐项细化时必须沿用的 benchmarking / perf 复盘口径，确保每一轮优化都能被同口径比较。
- 明确 backlog change 本身不直接承诺一次性实现全部优化，而是作为后续拆分分析、设计和实施的总入口。

## Capabilities

### New Capabilities
- `query-optimization-backlog`: 维护 query 主路径后续优化项的优先级、收益预估、profiling gate 和逐项落地入口。

### Modified Capabilities
- `query-pipeline`: 后续 query 优化项的分析与实施必须挂接到统一的主路径阶段拆分与热点归因口径。
- `e2e-benchmark`: 后续优化项的验收必须保持统一的 full E2E 与 query-only perf 输出口径。

## Impact

影响范围主要在 `openspec/changes/` 下的新 change 文档，以及后续可能继续修改的查询主路径、benchmark 输出和 perf 复盘流程。当前 change 先把 `probe_submit` 的首批三项优化写成可直接继续细化和实施的 backlog 工作包，不引入新依赖，不改变已有 CLI 或索引格式。
