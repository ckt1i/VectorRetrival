## 1. Canonical Baseline Switch

- [x] 1.1 审计所有 formal-study 入口、报告脚本和文档，找出当前仍把 `faiss_ivfrq.py` 当作 canonical `IVF-RQ` baseline 的位置。
- [x] 1.2 更新 canonical baseline 的选择逻辑，使主比较运行路由到 `baselines/formal-study/scripts/run_vector_search/ivf_rabitq_rerank.py`。
- [x] 1.3 保留 `faiss_ivfrq.py` 作为明确标注的 Faiss reference path，而不是主用的 `IVF+RQ+rerank` baseline。

## 2. Payload-Aligned Official RabitQ Execution

- [x] 2.1 检查 `third_party/ivf-rabitq-rerank-baseline` 的 build/search core，定位需要把 payload/original-data fetch 加入计时范围的具体位置。
- [x] 2.2 修改官方 RabitQ baseline 的 wrapper 和 / 或 C++ core，使 query latency 在一个对齐的计时口径下包含 compressed search、磁盘原始向量 rerank 和 payload fetch。
- [x] 2.3 导出结构化 metadata，明确记录 compressed-search kernel identity、原始向量访问模式、payload 计时范围和 candidate-budget。

## 3. Benchmark Infra And Reporting

- [x] 3.1 更新 benchmark-infra 的结果 schema / summary，使 canonical baseline identity 能区分 `official_rabitq_library` 和 `faiss_ivfrq_reference`。
- [x] 3.2 确保 operating-point summary 能同时报告 canonical 官方 baseline 的 `nprobe` 和 candidate-budget。
- [x] 3.3 更新所有面向论文或实验的标签，使主表中的 `IVF-RQ baseline` 解析到官方 RabitQ 路径。

## 4. COCO 100K Retune And Validation

- [ ] 4.1 为 COCO 100K 重建或验证官方 RabitQ baseline index，使用预期的 `nlist=2048` 配置和常驻内存的 compressed search。
- [ ] 4.2 在 COCO 100K 上以 `nprobe=64` 扫描 `candidate_budget` 和相关运行参数，使用 overlap `recall@10` 与包含 payload 的延迟口径。
- [ ] 4.3 选出并记录最佳的 `nprobe=64` 工作点，目标是让 `recall@10` 接近 `0.9`，或者显式标记该目标未达成。
- [ ] 4.4 重新运行最终的 canonical baseline 配置，并验证导出的指标、计时范围和 metadata 全部符合新的口径。
