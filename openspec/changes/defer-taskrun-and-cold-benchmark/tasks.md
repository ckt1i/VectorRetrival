## 1. DEFER_TASKRUN + SINGLE_ISSUER

- [x] 1.1 在 IoUringReader::Init() 中添加 DEFER_TASKRUN + SINGLE_ISSUER flags，包含 fallback 重试逻辑
- [x] 1.2 添加 defer_taskrun_enabled_ 成员变量，记录是否成功启用
- [x] 1.3 在 Poll() 中添加 DEFER_TASKRUN 适配：启用时先调用 io_uring_get_events() 再 peek CQE

## 2. bench_e2e 冷读模式

- [x] 2.1 为 bench_e2e 添加 --cold 命令行参数解析
- [x] 2.2 实现 posix_fadvise(FADV_DONTNEED) 驱逐 .clu 和 .dat page cache 的逻辑
- [x] 2.3 实现热读→驱逐→冷读的两轮查询流程，输出对比表

## 3. 验证

- [x] 3.1 编译并运行现有测试，确保 DEFER_TASKRUN 改动不破坏功能
- [x] 3.2 在 NVMe 上执行 bench_e2e --cold，记录热读 vs 冷读数据
