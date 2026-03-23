# Tasks: Benchmark Restructure

## 1. 目录重组 + CMake 隔离

- [x] 1.1 创建 `benchmarks/` 目录
- [x] 1.2 `git mv` 三个 bench_*.cpp 从 `tests/index/` 到 `benchmarks/`
- [x] 1.3 创建 `benchmarks/CMakeLists.txt`（VDB_BUILD_BENCHMARKS 控制，4 个 targets）
- [x] 1.4 根 CMakeLists.txt: 删除 VDB_BUILD_TESTS 块内的 bench_* targets
- [x] 1.5 根 CMakeLists.txt: 添加 `add_subdirectory(benchmarks)`
- [x] 1.6 验证: `cmake -DVDB_BUILD_BENCHMARKS=ON -DVDB_BUILD_TESTS=OFF` 只编译 benchmark
- [x] 1.7 验证: `cmake -DVDB_BUILD_TESTS=ON -DVDB_BUILD_BENCHMARKS=OFF` 不编译 benchmark，ctest 全通过 (35/35)

## 2. bench_e2e 参数化

- [x] 2.1 添加 `GetFloatArg` 辅助函数
- [x] 2.2 将硬编码参数替换为 CLI 参数（nlist, nprobe, topk, early-stop, bits, block-size, c-factor, max-iter, seed, page-size, p-for-dk, output）
- [x] 2.3 验证: 无参数调用时行为与之前一致（默认值与硬编码一致）
- [x] 2.4 验证: 编译通过无 warning

## 3. bench_conann_recall 新 benchmark

- [x] 3.1 实现 bench_conann_recall.cpp: 数据加载 + brute-force GT
- [x] 3.2 实现 K-Means 聚类 + d_k 校准（query/database 分离）
- [x] 3.3 实现 per-query 搜索循环: centroid 排序 → 逐聚类精确 L2 → top-k heap → early stop
- [x] 3.4 实现输出: 汇总表 + per-query 采样
- [x] 3.5 验证: `./bench_conann_recall --dataset coco_1k --nlist 16 --nprobe 8` → recall@10=0.6374, avg_probes=2.21, early_stop=96.2%

## 4. 最终验证

- [x] 4.1 编译无 warning（BENCHMARKS=ON, TESTS=ON）
- [x] 4.2 ctest 全部通过 (35/35)
- [x] 4.3 四个 benchmark 在 build/benchmarks/ 下正常构建
