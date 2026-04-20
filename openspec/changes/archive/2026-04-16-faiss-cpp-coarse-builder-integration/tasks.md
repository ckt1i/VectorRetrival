## 1. Faiss Build Integration

- [ ] 1.1 以 CPU-only 受限模式将 `thrid-party/faiss` 接入当前 CMake，显式关闭 GPU、Python、extras 和 testing
- [ ] 1.2 将 Faiss 的 `add_subdirectory` 放在现有基础库定义完成后、`vdb_index` 定义前，并确保它作为 `vdb_index` 的局部依赖接入而不是全局前置依赖
- [ ] 1.3 落实双标准构建策略：主工程继续保持 C++17，Faiss 子项目单独以 C++20 编译，并验证其标准设置不会污染现有非 Faiss targets
- [ ] 1.4 让 `vdb_index` 稳定链接 Faiss core library，同时通过局部 adapter 使用 Faiss 头文件，避免其扩散到其他模块
- [ ] 1.5 明确并记录 Faiss 子项目与主工程的 OpenMP、BLAS/LAPACK 依赖边界

## 2. Faiss Coarse Builder Path

- [ ] 2.1 为 `IvfBuilderConfig` 增加最小必要的 Faiss 训练参数，包括 train size、effective metric 口径和必要的迭代配置
- [ ] 2.2 新增 Faiss coarse builder helper / adapter，在 C++ 内完成 centroids 训练与 single nearest-centroid assignment
- [ ] 2.3 将 `IvfBuilder::RunKMeans()` 中的 `faiss_kmeans` 从 precomputed-only 报错路径改为可直接训练的正式 builder 分支
- [ ] 2.4 保留现有 precomputed centroids / assignments 导入能力，并将其与 `faiss_kmeans` builder identity 解耦

## 3. Metadata And Benchmark Integration

- [ ] 3.1 扩展 build metadata 输出，记录 coarse builder、clustering source、effective metric、Faiss train size 和迭代口径
- [ ] 3.2 更新 benchmark / e2e 配置与 summary 输出，使 `faiss_kmeans` 在无 precomputed 工件时也能自动构建并留下结构化 provenance
- [ ] 3.3 增加回归覆盖，比较 C++ Faiss builder 与现有 Python exporter 路径在相同配置下的 centroids / assignments 或 recall operating point 是否一致
