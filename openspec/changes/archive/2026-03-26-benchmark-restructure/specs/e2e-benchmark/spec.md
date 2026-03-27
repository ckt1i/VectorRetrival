# Delta Spec: E2E Benchmark — CLI 参数化

## 变更：输入参数改为 CLI 传入

### 新增 CLI 参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--dataset` | string | `/home/zcq/VDB/data/coco_1k` | 数据集路径（已有） |
| `--queries` | int | 0 | query 数量限制（已有） |
| `--output` | string | `/home/zcq/VDB/test/` | 输出基础目录 |
| `--nlist` | int | 32 | K-Means 聚类数 |
| `--nprobe` | int | 32 | 搜索探查聚类数 |
| `--topk` | int | 10 | Top-K |
| `--early-stop` | int | 1 | 启用 early stop (1=on, 0=off) |
| `--bits` | int | 1 | RaBitQ 量化位数 |
| `--block-size` | int | 64 | RaBitQ 块大小 |
| `--c-factor` | float | 5.75 | RaBitQ c 因子 |
| `--max-iter` | int | 20 | K-Means 最大迭代 |
| `--seed` | int | 42 | 随机种子 |
| `--page-size` | int | 4096 | 存储页大小 |
| `--p-for-dk` | int | 99 | d_k 校准百分位 |

### 行为约束

- 所有参数的默认值必须与当前硬编码值一致，确保无参数调用时行为不变
- 新增 `GetFloatArg` 辅助函数支持 float 类型参数
- config.json 输出中包含所有参数值（含默认值）
