## 1. RotationMatrix 序列化格式变更（不向后兼容）

- [x] 1.1 修改 `src/rabitq/rabitq_rotation.cpp` 的 `Save()`：在 dense data 之后写入 flags(u8)。当 `use_fast_hadamard_=true` 时 flags=1 并追加 diag_signs_(dim × int8)；否则 flags=0
- [x] 1.2 修改 `Load()`：读完 dense data 后读取 1 字节 flags。若 `flags & 1` 则读 dim 个 int8 diag_signs，设置 `use_fast_hadamard_=true` + `diag_signs_`；否则 `use_fast_hadamard_=false`
- [x] 1.3 更新 `include/vdb/rabitq/rabitq_rotation.h` 的注释：移除 "NOT persisted" 说明，改为描述新格式
- [x] 1.4 更新 `RotationMatrix(Dim dim, std::vector<float> data)` 构造函数，增加可选的 `use_fast_hadamard` 和 `diag_signs` 参数（或在 Load 中直接设置成员变量）

## 2. 编译验证

- [x] 2.1 在 `build-bench-only/` 重新编译：`cmake --build . -j$(nproc)` 确认无编译错误

## 3. 重建 COCO 100K 索引

- [x] 3.1 运行 `bench_e2e --dataset /home/zcq/VDB/data/coco_100k --queries 500`（不指定 --index-dir，触发完整 build），将输出目录记录为 NEW_INDEX_DIR
- [x] 3.2 验证 NEW_INDEX_DIR 包含 `rotation.bin`（新格式，大小 = 4 + dim×dim×4 + 1 + dim）
- [x] 3.3 验证 NEW_INDEX_DIR 包含 `rotated_centroids.bin`
- [x] 3.4 验证 bench_e2e 输出中 `used_hadamard` 状态正常（OverlapScheduler 走 PrepareQueryRotatedInto 路径）

## 4. 方案 A 效果验证

- [x] 4.1 在新索引上运行 `--nprobe-sweep 50,100,150,200 --queries 500 --index-dir NEW_INDEX_DIR`，保存 CSV
- [x] 4.2 对比修复前 CSV（`/home/zcq/VDB/test/coco_100k_20260412T175900/nprobe_sweep.csv`）：确认 recall@10 差异 < 0.005
- [x] 4.3 确认 probe_time_ms 显著下降（预期 nprobe=200 从 1.75ms 降至 < 0.5ms）
- [x] 4.4 分析新的 timing 分布：记录 uring_submit_ms、parse_cluster_ms、fetch_missing_ms 等各项占比
- [x] 4.5 运行 `sudo perf stat` 对比修复前的 IPC、cache-miss rate、L1-dcache-misses 变化
- [x] 4.6 根据 timing 分析结果判断方案 B（Submit 批量化）的预期收益，记录结论

## 5. IoUringReader prepped() 接口

- [x] 5.1 在 `include/vdb/query/async_reader.h` 的 `AsyncReader` 基类中添加 `virtual uint32_t prepped() const = 0;`
- [x] 5.2 在 `IoUringReader` 中实现 `uint32_t prepped() const override { return prepped_; }`
- [x] 5.3 在 `PreadFallbackReader` 中实现 `uint32_t prepped() const override { return static_cast<uint32_t>(pending_.size()); }`

## 6. SearchConfig 新增 submit_batch_size

- [x] 6.1 在 `include/vdb/query/search_context.h` 的 `SearchConfig` 中添加 `uint32_t submit_batch_size = 8;`
- [x] 6.2 在 `benchmarks/bench_e2e.cpp` 中添加 `--submit-batch N` 参数解析，设置到 `search_cfg.submit_batch_size`

## 7. ProbeAndDrainInterleaved Submit 批量化

- [x] 7.1 在 `ProbeAndDrainInterleaved` 中添加 `uint32_t clusters_since_submit = 0` 计数器
- [x] 7.2 将 step 4（post-probe Submit）改为条件 Submit：`clusters_since_submit++; if (config_.submit_batch_size == 0 || clusters_since_submit >= config_.submit_batch_size) { Submit(); clusters_since_submit = 0; }`
- [x] 7.3 将 step 5（refill Submit）合并：refill 后不单独 Submit，由 batch 逻辑统一管理
- [x] 7.4 在 WaitAndPoll 调用前添加 safety flush：`if (reader_.prepped() > 0) { Submit(); }`
- [x] 7.5 循环结束后（含 early_stop break 后）添加残余 flush：`if (reader_.prepped() > 0) { Submit(); }`

## 8. 编译验证 + 方案 B 效果测试

- [x] 8.1 重新编译确认无错误
- [x] 8.2 在新索引上运行 `--nprobe-sweep 50,100,150,200 --queries 500 --submit-batch 8`，保存结果
- [x] 8.3 对比方案 A 的 CSV：确认 recall 不变，uring_submit_ms 显著下降
- [x] 8.4 对比 `--submit-batch 0`（原行为）和 `--submit-batch 8` / `--submit-batch 16` 的 total 延迟，确认批量化实际收益
- [x] 8.5 如果批量化收益不显著（< 0.1ms），记录结论并考虑将默认 batch_size 设为 0（保持原行为）
