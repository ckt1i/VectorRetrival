## 1. Benchmark Control Plane

- [x] 1.1 将 `SearchConfig::io_queue_depth` 接通到 `IoUringReader::Init()`，移除 benchmark 中对 queue depth=64 的硬编码
- [x] 1.2 在 `bench_e2e` 中新增 submit-path 相关 CLI 参数，至少覆盖 `--io-queue-depth`、SQPOLL 开关和 submission mode 选择
- [x] 1.3 在 benchmark 输出中记录 effective queue depth、effective SQPOLL mode、submission mode 等配置元数据
- [x] 1.4 在 benchmark timing 输出中保留并核对 `uring_submit_ms` 等 submit-path 观测字段，确保 sweep 结果可比较

## 2. Single-Ring Submit Refactor

- [x] 2.1 重构 `OverlapScheduler` 的提交逻辑，减少 cluster refill 与 vec/payload 路径分离 submit 带来的重复 `io_uring_enter`
- [x] 2.2 在任何阻塞等待 cluster completion 之前实现统一 safety flush，确保 prepared SQE 不会因等待路径而悬空
- [x] 2.3 为 shared-ring 模式加入 cluster/data 的逻辑隔离策略（如保留槽位或独立阈值），避免 cluster submit 破坏 vec batching
- [x] 2.4 补充单 ring submit-path 的回归测试，验证搜索结果、顺序和 early-stop 语义不变

## 3. SQPOLL And Isolated Modes

- [x] 3.1 在 `IoUringReader` 中加入可选 SQPOLL 初始化路径，并保留失败回退与 effective-mode 可观测性
- [x] 3.2 在调度器中实现 isolated submission mode 的基础框架，支持 cluster I/O 与 data I/O 隔离运行
- [x] 3.3 为 isolated mode 补充 completion drain 与 teardown 逻辑，确保 mixed CQE、early stop 和 query 结束时无死锁或泄漏
- [x] 3.4 为 shared mode 与 isolated mode 增加一致性验证，确认同一查询下结果集合与排序一致

## 4. Completion Bookkeeping Optimization

- [x] 4.1 用 slot vector + freelist 替换 `pending_` 的指针哈希热路径，并将 slot 标识编码到 completion 关联状态中
- [x] 4.2 调整 buffer 生命周期与 orphan cleanup 逻辑，确保优化后的 bookkeeping 在 normal drain、early stop、final teardown 下都能正确释放资源
- [x] 4.3 评估并实现 fixed buffers 或 registered buffer slabs 的最小可行路径，为更深队列下的 data read 提供可复用 buffer 管理

## 5. Validation And Performance Gates

- [x] 5.1 运行 warm-path 基线复测，记录 queue depth `64/128/256/512` 下的 `uring_submit_ms`、总延迟与 submit 次数
- [x] 5.2 对比 non-SQPOLL 与 SQPOLL 在相同 workload 下的收益，判断是否达到 proposal 中的保守收益区间
- [x] 5.3 对比 shared-ring 与 isolated mode，判断单 ring 收敛后是否仍有必要保留双 ring 路径
- [x] 5.4 汇总最终结果并更新 change 文档，明确哪些优化达到保守目标、哪些达到积极目标、哪些仅带来微优化收益
