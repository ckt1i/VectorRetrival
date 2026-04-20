## 1. Resident Query Hotpath

- [x] 1.1 为 `full_preload + use_resident_clusters=1` 增加 resident query wrapper / scratch 复用路径，前移容量保留与 query 无关结构初始化。
- [x] 1.2 将 resident 查询的临时对象和中间容器收敛到可复用的固定布局，减少每次 query 的重复构造、清理和包装开销。
- [x] 1.3 在 resident + single assignment 条件下实现轻量候选提交组织路径，并保留回退到通用路径的能力。

## 2. FastScan Stage1 P0 Optimization

- [x] 2.1 细化并压缩 `PrepareQueryRotatedInto` 的主路径，减少 query prepare 阶段的长期中间态和不必要拷贝。
- [x] 2.2 优化单 bit FastScan 的 LUT 构建和 Stage1 分类主循环，重点覆盖 `BuildFastScanLUT` 与 `EstimateDistanceFastScan` 的常数项。
- [x] 2.3 保持 `probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms`、`probe_classify_ms`、`probe_submit_ms` 的统计边界稳定，避免优化过程中丢失观测口径。

## 3. Coarse Select Optimization

- [x] 3.1 为 `FindNearestClusters()` 或等价 coarse select 路径引入可复用的 coarse scratch，去掉 query 期间的临时分配与重建。
- [x] 3.2 收紧 coarse score buffer、cluster-id materialization 和相关数据布局，降低 centroid ranking 的写入、搬运和选择前后开销。
- [x] 3.3 保持 `coarse_select_ms` 独立输出，并在完成 3.1 / 3.2 后先补跑一次 profiling，用结果判断是否需要继续做 top-n selection 的第三步重写。

## 4. Benchmark And Reporting

- [x] 4.1 更新 `bench_e2e` 输出，确保 resident hotpath 优化后的 full E2E 与 query-only 模式都稳定输出细化时间字段。
- [x] 4.2 保持固定 resident 参数集作为本轮主验收口径：`coco_100k`、`nlist=2048`、`nprobe=64`、`full_preload`、`use_resident_clusters=1`、`early_stop=0`、`crc=1`。

## 5. Verification

- [x] 5.1 编译并执行同参数 full E2E 测试，记录 recall@1/@5/@10、avg/p50/p95/p99 和全部细化后的时间字段。
- [x] 5.2 在测试前确认 CPU 空闲后，执行同参数 `query-only` perf，记录新的 CPU 热点排序与主要占比。
- [x] 5.3 在 coarse select 前两步完成后，基于 profiling 判断 top-n selection 是否仍是主要剩余热点，并形成是否进入第三步的结论。
- [x] 5.4 对照优化前基线，总结 resident query wrapper、FastScan Stage1、coarse select 三部分各自带来的收益与剩余瓶颈。
