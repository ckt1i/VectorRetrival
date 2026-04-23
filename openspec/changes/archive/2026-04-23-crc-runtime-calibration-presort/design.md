## Context

当前 `CrcCalibrator::Calibrate` 的主要成本来自 `PickLambdaReg` 和 `BrentSolve` 期间反复评估 `FNR(lambda)`。现有实现里，`ComputePredictions` 在每次候选 `lambda` 评估时都会针对每条 query 重新构造 `(reg_score, step)` 数组、执行排序、选择 stop step，并重新构造 prediction 结果用于 overlap/FNR 统计。这导致 calibration 的复杂度被大量重复排序和重复小对象分配放大，已经成为开启 runtime CRC 后 preparation 阶段的主热点。

本 change 的约束是只优化计算组织方式，不改变 solver 语义。也就是说，`RegularizeScores`、候选 `kreg/reg_lambda` 搜索、Brent 求解、`CalibrationResults` 输出结构以及线上 `CrcStopper` 的消费方式都保持不变。离散候选阈值搜索被明确排除在本次设计之外，避免同时引入求解器语义变化和性能优化，影响归因清晰度。

## Goals / Non-Goals

**Goals:**
- 将 calibration 中“每次 `lambda` 评估都重新排序”的流程改成“每个 query 只预排序一次，后续多次二分查询”。
- 允许在预处理阶段构建 query-level stop profile，使 Brent 迭代期间只需要执行二分查找和轻量统计累加。
- 保持现有 Brent solver、`lamhat` 定义、`CalibrationResults` 字段和线上 stop 行为不变。
- 为后续是否切换离散候选阈值搜索保留独立实验空间；本次实现之后应当还能用旧口径直接对比 `lamhat`、FNR、avg_probed 和线上 recall/early-stop。

**Non-Goals:**
- 不切换到离散候选阈值搜索，不改变 `lambda` 的求解方式。
- 不修改 `crc_scores.bin` 格式，不引入新的持久化工件。
- 不修改线上 `CrcStopper::ShouldStop` 公式，也不处理 offline/online stop 语义是否完全一致的问题。
- 不把本次 change 扩展到 benchmark 输出格式以外的其他 query-path 优化。

## Decisions

### Decision: 用 query-level stop profile 取代重复排序
对每条 calibration query，在给定 `reg_lambda/kreg` 后先生成一次 `reg_score[p]`，并构造按 `reg_score` 升序排列的 stop profile。profile 至少包含：
- 排序后的 score 数组
- 排序位置到原始 step 的映射
- 该 step 对应的 probe 计数

这样在任意 `lambda` 下，系统只需对排序后的 score 做 `upper_bound`，即可得到与原实现一致的 stop step，而无需重新排序。

之所以选择这一方案，是因为 perf 已明确显示排序是 calibration 的主热点，而该改法保持了 stop 规则不变。相比直接改 solver，这一步的行为漂移更小，回归面更可控。

### Decision: 允许预计算 overlap / hit-count 统计，避免 Brent 内重复构造 prediction 集
在 stop profile 之外，calibration 预处理阶段可以为每条 query 的每个 step 预先计算与 GT 的 overlap 计数，或等价的命中统计。之后在 `FNR(lambda)` 评估中，只需根据 `selected_step` 读取预计算计数并累加，而不再在 Brent 循环内重复构造 `PredictionResult.predictions`、`std::set` 或等价集合。

选择这一方案是因为仅消除排序后，Brent 迭代中的 prediction 构造和 overlap 统计仍会保留明显常数成本。将这部分一起前移，能更完整地把 `lambda` 评估收敛为“二分 + 查表 + 累加”。

备选方案是只做预排序，不做 overlap 预计算。这样实现更小，但保留了大量重复小对象构造，预计无法充分释放 calibration 热点，因此不作为首选。

### Decision: 保留 Brent solver，先把性能优化和求解器替换解耦
优化后的 `FNR(lambda)` 仍然由 Brent 调用，只是其内部实现不再重复排序和重建 prediction。`PickLambdaReg` 仍使用当前候选搜索逻辑，`lamhat` 仍然作为连续实数输出。

之所以保留 Brent，是为了让第一步 change 的收益归因保持单一：本次只回答“同一求解语义下能否显著降低 calibration 成本”，不回答“离散候选阈值搜索是否比 Brent 更合理”。后者作为下一步独立 change 更合适。

### Decision: 验收以行为等价和 preparation 热点下降为主
本次实现完成后，主要验收信号不是单纯的 wall-clock 更快，而是：
- `lamhat` 与旧实现一致或仅有数值级微小偏差
- calibration eval 的 `actual_fnr`、`avg_probed`、`recall@10` 保持一致或近似一致
- 在线 benchmark 的 `recall@10`、`avg_probed_clusters`、`early_stop` 行为不出现非预期漂移
- perf 中 `ComputePredictions + sort` 热点显著下降

之所以这样定义验收，是因为 calibration 是一个求解器内环，单看耗时下降不足以证明没有破坏 stop 语义。

## Risks / Trade-offs

- [排序语义与原实现不完全一致] → 保持“同一排序键、同一 `last <= lambda` 规则”，并用固定 `crc_scores.bin` 对比新旧 `lamhat` 与 eval 结果。
- [预计算 overlap 增加内存占用] → 只为 calib/tune/test 当前批次保留紧凑计数，而不是复制完整 prediction 结构。
- [重复值或边界点导致二分选中 step 与旧实现不一致] → 明确使用与旧实现一致的稳定规则，例如 `upper_bound` 取最后一个 `<= lambda` 的位置。
- [一次改动同时牵涉 `PickLambdaReg`、Brent 目标函数和统计结构，回归面偏大] → 分层重构：先抽出 query profile，再替换 `ComputePredictions` / `FalseNegativeRate` 的内部实现，最后补回归 benchmark。
- [保留 Brent 后仍有剩余开销] → 接受这是第一步优化的边界；若 perf 仍显示 solver 本身显著占比，再单独提离散候选阈值搜索 change。
