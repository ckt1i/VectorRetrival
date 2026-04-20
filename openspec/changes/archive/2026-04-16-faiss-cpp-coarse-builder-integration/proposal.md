## Why

当前仓库虽然已经能以 `faiss_kmeans` 名义做 coarse-builder 对照，但实际仍依赖 Python 脚本预先导出 `.fvecs/.ivecs` 再由 `IvfBuilder` 导入。这条路径已经不足以支撑后续把 Faiss 作为正式 single-mode coarse builder 收敛进主实验仓库，因为它把 builder identity、artifact source 和实验参数管理混在了一起，复现与默认化都不稳定。

## What Changes

- 新增一条 CPU-only 的 Faiss C++ coarse builder 集成路径，把官方 `thrid-party/faiss` 作为子项目接入当前仓库构建系统。
- 明确采用双标准策略：主工程继续保持 C++17，Faiss 子项目单独以 C++20 编译；本 change 需要控制 CMake 集成边界，避免 Faiss 的语言标准设置污染现有 target。
- 明确采用后置 CMake 接入策略：Faiss 不作为全局前置依赖接入，而是在现有基础库定义完成后、`vdb_index` 定义开始前，以受限子项目方式插入，只服务 `vdb_index` 及其 Faiss adapter。
- 将 `faiss_kmeans` 从“仅支持 precomputed artifacts 导入”的旁路，升级为 `IvfBuilder` 内可直接训练 centroids 与 primary assignments 的正式 coarse builder。
- 保留现有 `.clu` / `.dat` / `segment.meta` / CRC / query / rerank 主路径不变；Faiss 在本 change 中只负责 coarse clustering 与 single nearest-centroid assignment。
- 扩展 build metadata 与 benchmark bookkeeping，使 `faiss_kmeans` 的 training source、metric 归一化、train size、iteration 口径能够被结构化记录。
- 保持现有 precomputed centroids / assignments 导入能力，用于回归对照与 artifact reuse；但新设计 SHALL 将 `precomputed` 与 `faiss_kmeans` 解耦为两个独立维度。

## Capabilities

### New Capabilities
- `faiss-cpp-coarse-builder`: 在 `IvfBuilder` 内通过 Faiss C++ 直接训练 IVF coarse centroids 并生成 single nearest-centroid assignments，作为正式的 `faiss_kmeans` builder 路径。

### Modified Capabilities
- `benchmark-infra`: benchmark 与 build sidecar 输出需要记录 coarse builder provenance、Faiss 训练参数和 artifact source，以支撑正式实验与回归比较。

## Impact

- 影响代码：顶层 `CMakeLists.txt` 中 Faiss 的插入位置与 option forcing、`vdb_index` 依赖、Faiss adapter、`IvfBuilderConfig`、`IvfBuilder::RunKMeans()` 分派逻辑、build metadata 写出、benchmark 配置与 summary 元数据。
- 影响依赖：新增官方 Faiss C++ 仓库作为 `thrid-party/faiss` 子项目，第一版仅启用 CPU core library，禁用 GPU / Python / extras，并要求主工程 C++17 与 Faiss C++20 的标准边界可被稳定管理。
- 影响实验系统：`faiss_kmeans` 不再强依赖 Python exporter 才能构建；formal-study 与 e2e benchmark 可直接以 builder 配置复跑 Faiss coarse clustering。
