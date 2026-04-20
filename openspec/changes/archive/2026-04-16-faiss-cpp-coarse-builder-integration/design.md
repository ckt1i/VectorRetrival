## Context

当前仓库已经完成了 `faiss_kmeans` single-builder parity 对照，但这条路径本质上仍是“外部 Python 预计算工件 + C++ 导入”的实验旁路，而不是 build system 内的正式 coarse builder。与此同时，用户已经决定后续 single-mode IVF clustering 优先复用 Faiss 路线，而不是继续围绕 SuperKMeans family 做复杂的 retrieval-aware clustering 改造。

仓库当前存在三个明确约束：

1. `IvfBuilder` 仍是 build orchestration 的中心，负责 clustering、CRC calibration、RaBitQ encode 与写盘；本 change 不应打散这一契约。
2. 官方 Faiss 仓库默认配置过重：GPU=ON、Python=ON、extras=ON、MKL=ON、C++20；若原样接入，容易污染主工程构建与依赖边界。
3. 现有实验结论表明当前主矛盾是 coarse builder，而不是 query / rerank 主路径；因此本 change 需要避免把 full Faiss serving stack 一起引入。

## Goals / Non-Goals

**Goals:**
- 以 `thrid-party/faiss` 官方仓库为基础，把 Faiss CPU C++ core library 接入当前工程构建系统。
- 让 `faiss_kmeans` 成为 `IvfBuilder` 内可直接运行的正式 coarse builder，而不是仅支持导入预计算工件的旁路。
- 保持 `IvfBuilder` 对外 build contract 不变：Faiss 仅替换 coarse clustering 与 primary assignment，其余 build / serving 路径保持现状。
- 将 builder identity 与 artifact source 解耦：`faiss_kmeans` 描述 builder family，`precomputed` 描述 clustering source。
- 在 build metadata 与 benchmark 输出中记录 Faiss 训练口径，保证实验可复跑、可比较。

**Non-Goals:**
- 本 change 不接入 Faiss GPU 路径，不处理 CUDA / ROCm / cuVS。
- 本 change 不将查询阶段 coarse probing、candidate generation 或 rerank 迁移到 Faiss。
- 本 change 不移除现有 `superkmeans` / `hierarchical_superkmeans` builder，也不立即修改默认 `Auto` 选择策略。
- 本 change 不要求第一版就暴露 Faiss 所有高级训练参数；只收敛最小必要参数以复现当前实验口径。

## Decisions

### 1. 以 CPU-only 子项目方式接入官方 Faiss，而不是原样启用所有官方选项
Faiss SHALL 以 `thrid-party/faiss` 子目录接入当前 CMake，但必须显式关闭 GPU、Python、C API、extras 和 testing，仅构建 CPU core library。

原因：
- 当前项目只需要 `Clustering + IndexFlat + MetricType` 等 CPU builder 能力，不需要官方 demos、Python binding 或 GPU code path。
- 受限接入能把依赖和编译复杂度控制在 `vdb_index` 边界内，避免把整个工程拖入 CUDA / Python / extras。

备选方案：
- 原样 `add_subdirectory(thrid-party/faiss)`：否决，因为默认选项过重，工程风险过大。
- 继续依赖 Python exporter：否决，因为无法把 `faiss_kmeans` 正式收敛为仓库内 builder。

### 2. Faiss 采用后置局部 CMake 接入，而不是像基础依赖一样前置引入
设计 SHALL 将 Faiss 的 `add_subdirectory` 放在当前基础库定义完成之后、`vdb_index` 定义开始之前。Faiss SHALL 作为局部子项目只服务 `vdb_index` 与其 coarse-builder adapter，而 SHALL NOT 像 `SuperKMeans` 那样在依赖区最前面作为全局基础依赖接入。

原因：
- 官方 Faiss 顶层会设置 `CMAKE_CXX_STANDARD=20` 并引入 OpenMP / BLAS / LAPACK 搜索逻辑；若接入位置过早，污染面会扩散到主工程的大量非 Faiss targets。
- 将 Faiss 插在 `vdb_storage` 之后、`vdb_index` 之前，可以最大化保护 `vdb_common`、`vdb_simd`、`vdb_rabitq`、`vdb_storage` 等基础库不受其目录级默认值影响。
- 当前唯一直接需要 Faiss 的模块是 coarse builder，因此 Faiss 最合理的角色是 `vdb_index` 的局部依赖，而不是整个工程的前置公共依赖。

备选方案：
- 在依赖区与 `SuperKMeans` 一起前置 `add_subdirectory`：否决，因为污染范围过大。
- 在文件最末尾再接入 Faiss：否决，因为 `vdb_index` 定义时就需要 Faiss target。

### 3. 主工程保持 C++17，Faiss 子项目单独使用 C++20
设计 SHALL 以双标准模式集成 Faiss：当前主工程及现有 VDB targets 保持 C++17，官方 Faiss 子项目按其自身要求以 C++20 编译。CMake 集成 SHALL 控制语言标准影响范围，避免 Faiss 顶层标准设置污染现有非 Faiss targets。

原因：
- 当前 change 的目标是接入新的 coarse builder，而不是推动全工程语言标准升级。
- 全工程直接升到 C++20 会把影响面扩大到所有模块、第三方依赖和现有 benchmark，超出本 change 的必要范围。
- 只要 Faiss public headers 可被当前 builder adapter 稳定消费，主工程与 Faiss 分别使用 C++17/C++20 是最小侵入方案。

备选方案：
- 全工程统一升级到 C++20：暂缓，只有在双标准方案无法稳定成立时才评估。
- 强行修改 Faiss 使其降回 C++17：否决，因为这会增加第三方维护成本并偏离官方构建假设。

### 4. Faiss 仅作为 coarse builder backend，不替换 `IvfBuilder` 的后续 build contract
`faiss_kmeans` 路径 SHALL 只在 `IvfBuilder::RunKMeans()` 内负责 centroids training 与全量 primary nearest-centroid assignment。其后仍使用现有 `DeriveSecondaryAssignments()`、`CalibrateDk()` 与 `WriteIndex()`。

原因：
- 当前要解决的是 coarse builder parity，而不是 full system parity。
- 这能最大化复用现有存储格式、CRC 标定、RaBitQ 编码和查询链路，保持变量隔离。

备选方案：
- 直接引入 Faiss IVF search / query stack：否决，因为会把 builder 和 serving 差异重新缠在一起。

### 5. 将 `precomputed` 与 `faiss_kmeans` 解耦为 source 与 builder 两个维度
设计 SHALL 明确区分：
- `coarse_builder`: `superkmeans` / `hierarchical_superkmeans` / `faiss_kmeans`
- `clustering_source`: `auto` / `precomputed`

即使是 `faiss_kmeans`，也允许同时存在：
- `clustering_source=auto`: 由 C++ Faiss 直接训练
- `clustering_source=precomputed`: 导入外部生成工件

原因：
- 这是当前方案里最关键的语义清理。否则 builder family 和 artifact source 会继续混淆。
- 这允许保留已有 Python exporter 作为回归和 artifact reuse 路径，而不阻碍正式 builder 集成。

备选方案：
- 让 `faiss_kmeans` 仅代表 precomputed mode：否决，因为这会继续维持当前架构缺陷。

### 6. 第一版只暴露最小必要的 Faiss 训练参数
第一版 Faiss config SHALL 最少补齐：
- training sample size
- iteration count
- seed
- effective metric / cosine normalization mode
- 可选的 `nredo`

原因：
- 这些参数直接决定结果复现性，且与现有 Python exporter 口径强相关。
- 过早暴露全部 `ClusteringParameters` 会把接口变复杂，不利于收敛。

备选方案：
- 完全隐藏 Faiss 参数：否决，因为实验不可复现。
- 一次暴露全部 Faiss 参数：暂缓，因为第一版不需要这么宽的 API 面。

## Risks / Trade-offs

- **[Risk] Faiss 顶层 CMake 的 C++20 / OpenMP / BLAS 要求污染主工程 target** → Mitigation: 采用“后置局部接入 + 双标准模式”作为默认设计，把 Faiss 限制在 `vdb_storage` 与 `vdb_index` 之间接入，并优先验证其影响不扩散到现有基础库与非 Faiss targets。
- **[Risk] C++ Faiss builder 与现有 Python exporter 的归一化/metric 语义不一致，导致结果回归困难** → Mitigation: 第一版明确复刻现有 cosine→IP 归一化口径，并把 effective metric 写入 metadata。
- **[Risk] `faiss_kmeans` 接入后 bench 仍隐式依赖 precomputed artifacts，导致语义未真正收敛** → Mitigation: benchmark 输入必须显式区分 builder 和 source，并在无 precomputed 输入时允许自动训练。
- **[Risk] 未来若切默认 builder，现有 `Auto` 行为回归风险大** → Mitigation: 本 change 不改默认策略，只先完成 builder 集成与对照。

## Migration Plan

1. 在现有基础库定义完成后、`vdb_index` 定义开始前，以 CPU-only 方式将官方 Faiss 仓库作为局部子项目接入。
2. 优先验证“主工程 C++17 / Faiss C++20”的双标准构建边界，以及 Faiss 的目录级设置不会污染现有非 Faiss targets。
3. 在 `vdb_index` 中通过局部 adapter 使用 Faiss，并避免其他模块直接 include Faiss 头文件。
4. 在 `IvfBuilderConfig` 中补齐最小必要的 Faiss 参数，并新增 Faiss coarse builder helper / adapter。
5. 将 `IvfBuilder::RunKMeans()` 中的 `faiss_kmeans` 从 “InvalidArgument” 改为可执行训练路径，同时保留 precomputed import。
6. 补齐 build metadata / benchmark 元数据，确保 builder/source/metric/train-size 可见。
7. 用现有单层 parity 和 formal-study 对照链路回归验证：
   - C++ Faiss builder 与现有 Python exporter 结果是否一致或近似
   - `faiss_kmeans` 是否在无 precomputed 输入时可直接复跑

回滚策略：
- 保留现有 Python exporter 与 precomputed import 路径；
- 保留 `superkmeans` / `hierarchical_superkmeans` 与现有 `Auto` 行为；
- 若 Faiss 子项目集成导致构建不稳定，可临时关闭该 builder path 而不影响其余系统。

## Open Questions

- 第一版是否需要在 `IvfBuilderConfig` 中暴露 `nredo`，还是先固定为与当前 Python 路径一致的默认值？
- benchmark summary 中是否需要额外记录 `faiss_backend=cpu` 字段，为后续可能的 GPU 扩展预留口径？
