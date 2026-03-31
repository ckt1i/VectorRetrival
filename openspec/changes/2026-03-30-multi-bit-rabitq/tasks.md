# Tasks: Multi-bit RaBitQ 量化与两阶段查询

## Task 1: RaBitQEncoder 支持 M-bit 编码

**文件**: `include/vdb/rabitq/rabitq_encoder.h`, `src/rabitq/rabitq_encoder.cpp`

- [ ] RaBitQCode 结构添加 `uint8_t bits` 字段
- [ ] RaBitQEncoder 构造函数添加 `uint8_t bits = 1` 参数
- [ ] Encode() 实现 M-bit 均匀量化: `bin = clamp(floor((v+1)/2 × 2^M), 0, 2^M-1)`
- [ ] Encode() 实现 bit-plane 布局打包: planes[0]=MSB, planes[M-1]=LSB
- [ ] 验证: bits=1 时 code 布局与修改前一致
- [ ] num_code_words() 返回 `bits * ceil(dim/64)`
- [ ] 添加 `bits()` 访问器

## Task 2: RaBitQEstimator 支持 M-bit LUT 距离估算

**文件**: `include/vdb/rabitq/rabitq_estimator.h`, `src/rabitq/rabitq_estimator.cpp`

- [ ] PreparedQuery 添加 `uint8_t bits` 和 `std::vector<float> lut` 字段
- [ ] RaBitQEstimator 构造函数添加 `uint8_t bits = 1` 参数
- [ ] PrepareQuery() 当 bits>1 时预计算 LUT: `LUT[v] = (-1 + (2v+1)/2^M) / √L`
- [ ] 新增 EstimateDistanceMultiBit(): 从 bit-plane 提取 M-bit 值，LUT scan 计算内积
- [ ] 新增 EstimateDistanceMultiBitRaw(): 零拷贝版本（接受 raw code_words 指针）
- [ ] EstimateDistance (Stage 1) 保持不变，始终用 MSB plane

## Task 3: 单元测试

**文件**: `tests/rabitq/rabitq_encoder_test.cpp`, `tests/rabitq/rabitq_estimator_test.cpp`

- [ ] 测试 bits=1 编码结果与修改前一致
- [ ] 测试 bits=2 编码: MSB plane 等价于 bits=1 编码
- [ ] 测试 bits=4 编码: MSB plane 等价于 bits=1 编码
- [ ] 测试 bits=2 的 EstimateDistanceMultiBit 误差 < EstimateDistance
- [ ] 测试 bits=1 时 LUT 不被填充（或不被使用）
- [ ] 测试量化边界: v=-1, v=0, v=1 的 bin 值正确

## Task 4: bench_rabitq_accuracy 两阶段统计

**文件**: `benchmarks/bench_rabitq_accuracy.cpp`

- [ ] 添加 `--bits` CLI 参数（默认 1）
- [ ] 使用带 bits 参数的 Encoder/Estimator
- [ ] 实现两阶段分类: S1 (MSB popcount) → S2 (M-bit LUT, 仅 Uncertain_S1)
- [ ] 计算 margin_s2 = margin_s1 / 2^(bits-1)
- [ ] 记录 S1 的 SafeIn/SafeOut/Uncertain + False SafeIn/False SafeOut
- [ ] 记录 S2 的 SafeIn/SafeOut/Uncertain + False SafeIn/False SafeOut
- [ ] 输出 S1/S2 双阶段统计报告
- [ ] 输出 M-bit LUT 距离误差统计（与精确距离对比）
- [ ] bits=1 时输出与修改前一致（无 S2 统计）

## Task 5: bench_vector_search 两阶段 + False SafeIn/Out

**文件**: `benchmarks/bench_vector_search.cpp`

- [ ] 添加 `--bits` CLI 参数（默认 1）
- [ ] 查询循环集成两阶段逻辑 (S1 → S2 for Uncertain)
- [ ] 实现 I/O 语义: SafeIn→读 vec+payload, Uncertain→仅读 vec+rerank+确认后读 payload
- [ ] 新增 False SafeIn/Out 统计（对照 GT top-K）
- [ ] 分别输出 S1/S2 各阶段统计
- [ ] JSON 输出包含 false_safein_rate, false_safeout_rate
- [ ] bits=1 时行为与修改前一致（无 S2，但新增 False 统计）

## Task 6: 验证运行

- [ ] bits=1 运行 bench_rabitq_accuracy，确认结果与修改前一致
- [ ] bits=2 运行 bench_rabitq_accuracy，确认 recall@10 优于 bits=1
- [ ] bits=2 运行 bench_vector_search，确认 False SafeOut 统计正常输出
- [ ] 确认 bits=2 下 S2 的 Uncertain 比例显著低于 S1 的 Uncertain 比例
