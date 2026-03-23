## Context

现有 3 个 benchmark（bench_e2e, bench_rabitq_accuracy, bench_ivf_quality）在 `tests/index/` 目录下，和 unit test 共享 `VDB_BUILD_TESTS` 开关。`VDB_BUILD_BENCHMARKS` option 已声明但未接入任何 target。bench_e2e 有 ~15 个参数硬编码在源码中。

## Goals / Non-Goals

**Goals:**
- benchmark 可独立编译，不触发 unit test 编译
- `cmake --build build --target bench_xxx` 秒级增量编译
- bench_e2e 所有关键参数可通过 CLI 传入
- 新增 bench_conann_recall 评估 IVF 分区 + d_k early stop 的 recall 天花板

**Non-Goals:**
- 不提取 benchmark 共享代码库（各 bench 保持独立）
- 不改变 unit test 的目录结构或编译方式
- 不引入 benchmark framework（如 Google Benchmark）

## Decisions

### 1. 目录结构

```
VectorRetrival/
├── benchmarks/
│   ├── CMakeLists.txt          ← 受 VDB_BUILD_BENCHMARKS 控制
│   ├── bench_e2e.cpp
│   ├── bench_rabitq_accuracy.cpp
│   ├── bench_ivf_quality.cpp
│   └── bench_conann_recall.cpp
├── tests/                      ← 纯 unit tests，不变
└── CMakeLists.txt              ← add_subdirectory(benchmarks)
```

### 2. CMake 构建隔离

根 CMakeLists.txt 改动：
- 从 `VDB_BUILD_TESTS` 块中删除 bench_* 的 `add_executable` + `target_link_libraries`
- 在 `VDB_BUILD_TESTS` 块之后添加 `add_subdirectory(benchmarks)`

benchmarks/CMakeLists.txt：
- 顶部 `if(NOT VDB_BUILD_BENCHMARKS) return() endif()`
- 每个 bench 各自 `add_executable` + `target_link_libraries`

使用方式：
```bash
# 只编译 benchmark
cmake -B build -DVDB_BUILD_BENCHMARKS=ON -DVDB_BUILD_TESTS=OFF
cmake --build build

# 只编译单个 benchmark（库不变则秒级）
cmake --build build --target bench_conann_recall

# 全量编译
cmake -B build -DVDB_BUILD_BENCHMARKS=ON -DVDB_BUILD_TESTS=ON
cmake --build build
```

### 3. bench_e2e 参数化

复用 bench_rabitq_accuracy 的 `GetArg/GetIntArg` 模式，新增 CLI 参数：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--dataset` | string | coco_1k 路径 | 数据集路径 |
| `--queries` | int | 0 (all) | query 数量限制 |
| `--output` | string | /home/zcq/VDB/test/ | 输出目录 |
| `--nlist` | int | 32 | 聚类数 |
| `--nprobe` | int | 32 | 探查聚类数 |
| `--topk` | int | 10 | Top-K |
| `--early-stop` | int | 1 | 是否启用 early stop |
| `--bits` | int | 1 | RaBitQ 量化位数 |
| `--block-size` | int | 64 | RaBitQ 块大小 |
| `--c-factor` | float | 5.75 | RaBitQ c 因子 |
| `--max-iter` | int | 20 | K-Means 迭代数 |
| `--seed` | int | 42 | 随机种子 |
| `--page-size` | int | 4096 | 页大小 |
| `--p-for-dk` | int | 99 | d_k 百分位 |

### 4. bench_conann_recall 设计

纯 CPU 精确距离 benchmark，不使用 RaBitQ：

**流程：**
1. 加载 COCO-1k 数据集（image + query embeddings）
2. 暴力搜索计算 ground truth top-K
3. K-Means 聚类 image embeddings
4. 校准 d_k: `CalibrateDistanceThreshold(queries, Q, images, N, dim, ...)`
5. 对每个 query：
   - FindNearestClusters 获取最近的 nprobe 个聚类
   - 按聚类距离顺序逐个 probe，计算精确 L2² 到聚类内所有向量
   - 维护 top-k heap
   - Early stop: top-k 满且 worst_dist < d_k 时停止
6. 输出 per-query recall + 聚类 probe 数统计

**CLI 参数：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--dataset` | coco_1k 路径 | 数据集 |
| `--nlist` | 16 | 聚类数 |
| `--nprobe` | 8 | 最大 probe 数 |
| `--topk` | 10 | Top-K |
| `--p-for-dk` | 99 | d_k 百分位 |
| `--queries` | 0 (all) | query 数量限制 |

**输出格式（stdout）：**
- 汇总表：recall@1/5/10, avg_probes, early_stop_rate
- per-query 采样（每 20 条取 1 条）

## Risks / Trade-offs

- **文件移动**: `tests/index/bench_*.cpp` 路径变化，git history 断裂（用 `git mv` 保留）
- **参数默认值**: bench_e2e 参数化后默认值保持和之前硬编码一致，确保无参数时行为不变
- **bench_conann_recall 不走 RaBitQ**: 有意为之，目的是隔离 IVF 分区质量
