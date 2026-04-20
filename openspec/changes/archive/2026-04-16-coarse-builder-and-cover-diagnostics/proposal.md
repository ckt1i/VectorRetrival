## Why

当前实验表明，在 `coco_100k` 上做纯 IVF+Flat 探测时，仓库现有 IVF 聚类若想达到 99% 的 top-10 coarse cover，通常需要探测 300 个以上的聚类；而 `IVF+RQ+rerank` baseline 在更小的 probe budget 下就能达到相近覆盖率。基于现有证据，问题来源尚不清楚：既可能来自当前 coarse builder，也可能来自当前 redundant assignment 策略，或者两者同时存在缺陷。

因此，下一步需要先把 coarse builder 对照与 pure coarse-cover 诊断标准化，在继续做 serving 侧调优之前，先回答“问题到底出在 builder 还是 overlap 设计”这一核心问题。没有 builder 无关的诊断链路，后续对 overlap assignment 或查询调度的修改都很难被准确解释。

## What Changes

- 增加可配置的 coarse-builder 对照工作流，能够用多个 coarse builder 构建兼容的 IVF coarse partition，初始至少支持当前 SuperKMeans 路径和 Faiss KMeans 路径。
- 增加 pure coarse-cover 诊断能力，测量在 IVF+Flat 探测语义下，为覆盖 GT top-k 需要探测多少 coarse clusters；该诊断不依赖 CRC、epsilon pruning 或 query-time early stop。
- 为 `single`、`redundant_top2_naive`、`redundant_top2_rair` 三种 assignment mode 输出可直接对照的诊断结果，以区分 builder 质量与 overlap 质量。
- 增加实验输出，导出 coarse-cover 曲线以及派生 operating point，例如达到 `0.95`、`0.99`、`0.995` cover 所需的最小 `nprobe`。
- 保持现有 serving benchmark 不变；本 change 仅引入诊断与对照能力，不直接修改 serving 逻辑。

## Capabilities

### New Capabilities
- `coarse-builder-evaluation`: 使用多个 coarse builder 构建兼容的 IVF coarse partition，并在输出与元数据中记录 builder identity。
- `coarse-cover-diagnostics`: 在 builder 和 assignment mode 可控的条件下，测量 pure IVF coarse-cover 行为，并导出可比较的 cover-vs-probe 结果。

### Modified Capabilities
- `benchmark-infra`: 扩展 benchmark manifest 与输出格式，以支持 coarse-builder 诊断运行和结构化 coarse-cover 汇总。

## Impact

- 影响代码：IVF builder 配置、索引构建脚本、coarse-cover benchmark 工具、formal-study 诊断输出，以及实验 summary 聚合脚本。
- 影响系统：builder 选择、离线聚类评估、诊断 CSV/JSON/SVG 输出，以及引用 coarse-cover 证据的实验跟踪文档。
- 依赖项：Faiss coarse-builder 路径将作为离线 builder 对照的一等依赖引入。
