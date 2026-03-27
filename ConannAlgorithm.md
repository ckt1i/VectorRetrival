# 第一部分：离线校准算法（训练阶段）
## 目标
输入：用户指定的 **FNR 上限 α**、IVF 索引、校准查询集  
输出：最优早停阈值 $\hat{\lambda}$、RAPS 正则化参数 $\gamma$ 和 $c_{reg}$、全局归一化常数 **MAX_DISTANCE**

---

## 核心步骤（对应原文 Algorithm 1）
### 步骤 1：准备数据与初始化
#### 输入
- 已训练好的 IVF 索引（含 $P$ 个聚类，记为 `nlist`）
- 校准查询集 $Q_{calib}$（共 $M$ 个查询，记为 `calib_queries`）
- 每个校准查询的**精确 top-K 近邻 ID 列表**（记为 `gt_ids[q]`，用扁平索引计算，作为漏检判断的真值）
- 用户指定的 FNR 上限 $\alpha$

#### 初始化
- 预分配存储：
  - `nonconf[q][p]`：查询 $q$ 探测 $p$ 个聚类后的非一致性分数
  - `preds[q][p]`：查询 $q$ 探测 $p$ 个聚类后的 top-K ID 列表
  - `MAX_DISTANCE`：全局最大第 K 近邻距离（初始为 0）

---

### 步骤 2：compute_scores() —— 计算非一致性分数与全局 MAX_DISTANCE
这是你之前提到的核心函数，**强制探测所有聚类**，记录每一步的结果。

#### 伪代码
```python
def compute_scores(ivf_index, calib_queries, gt_ids, nlist, K):
    M = len(calib_queries)
    nonconf = [[0.0 for _ in range(nlist)] for _ in range(M)]
    preds = [[[] for _ in range(nlist)] for _ in range(M)]
    MAX_DISTANCE = 0.0

    for q_idx in range(M):
        q = calib_queries[q_idx]
        # 1. 强制探测所有聚类（nprobe = nlist），按质心距离由近到远
        all_results = ivf_index.search(q, nprobe=nlist, k=K)  # 返回 (distances, ids)，按探测顺序增量合并
        # 2. 逐次模拟探测 1..nlist 个聚类
        for p in range(1, nlist+1):
            # 取前 p 个聚类的合并结果，得到当前 top-K
            current_dists, current_ids = get_topk_from_first_p_clusters(all_results, p, K)
            # 记录当前 top-K 的第 K 近距离 score_k
            score_k = current_dists[-1]  # 假设按距离升序排列，最后一个是第 K 近
            # 更新全局 MAX_DISTANCE
            if score_k > MAX_DISTANCE:
                MAX_DISTANCE = score_k
            # 计算非一致性分数（原文简化版：直接除以 MAX_DISTANCE）
            nonconf[q_idx][p-1] = score_k / MAX_DISTANCE  # p-1 是数组索引
            # 记录当前 top-K ID 列表
            preds[q_idx][p-1] = current_ids

    return nonconf, preds, MAX_DISTANCE
```

---

### 步骤 3：调优 RAPS 正则化参数 $\gamma$ 和 $c_{reg}$
通过**网格搜索**在小部分校准数据上调优，目标是**最小化平均探测聚类数**，同时保证后续结合 $\lambda$ 仍满足 FNR 约束。

#### 网格搜索范围
- $\gamma$：[0.01, 0.05, 0.1, 0.2]（原文常用 0.1）
- $c_{reg}$：[0.3*nlist, 0.5*nlist, 0.7*nlist]（原生 IVF 固定探测数的 30%~70%）

#### 调优逻辑
对每组 $(\gamma, c_{reg})$：
1. 在 1000 个未见过的校准查询上模拟探测；
2. 统计平均探测聚类数；
3. 选择**平均探测数最小**的那组 $(\gamma, c_{reg})$。

---

### 步骤 4：优化最优早停阈值 $\hat{\lambda}$
以 $\alpha$ 为目标，遍历候选 $\lambda$，结合 CRC 有限样本修正项，选择满足约束的**最大 $\lambda$**（最大化 $\lambda$ 可更早停止）。

#### 伪代码
```python
def calibrate_lambda(nonconf, preds, gt_ids, M, alpha, gamma, c_reg, nlist, K):
    # 1. 离散化生成候选 λ（0.01~1.0，步长 0.01）
    candidate_lambdas = [i/100 for i in range(1, 101)]
    best_lambda = 0.0
    best_risk = float('inf')

    for lam in candidate_lambdas:
        total_miss = 0
        # 2. 对每个校准查询，模拟在 λ 下的早停
        for q_idx in range(M):
            # 找到满足早停条件的最小 p
            stop_p = nlist  # 默认探测所有
            for p in range(1, nlist+1):
                # 计算正则化非一致性分数
                pi = nonconf[q_idx][p-1]
                reg_term = gamma * max(0, p - c_reg)
                hat_pi = (1 - pi) + reg_term
                # 早停条件
                if hat_pi <= lam:
                    stop_p = p
                    break
            # 3. 判断是否漏检
            pred_ids = preds[q_idx][stop_p-1]
            gt = gt_ids[q_idx]
            # 计算交集大小
            intersection = len(set(pred_ids) & set(gt))
            if intersection < K:
                total_miss += 1
        # 4. 计算经验 FNR + 有限样本修正项
        empirical_risk = total_miss / M
        corrected_risk = empirical_risk + 1/(M+1)  # B=1，损失上界
        # 5. 筛选满足约束的最大 λ
        if corrected_risk <= alpha and lam > best_lambda:
            best_lambda = lam
            best_risk = corrected_risk

    return best_lambda
```

---

## 离线校准最终输出
- $\hat{\lambda}$：最优早停阈值
- $\gamma, c_{reg}$：RAPS 正则化参数
- `MAX_DISTANCE`：全局归一化常数

---

# 第二部分：在线查询早停算法（搜索阶段）
## 目标
输入：新查询向量 $x$、检索近邻数 $K$、离线校准输出的参数  
输出：满足 FNR ≤ α 的 top-K 近邻结果，且动态早停

---

## 核心步骤（对应原文 Algorithm 2）
### 步骤 1：初始化
加载离线校准参数：$\hat{\lambda}$、$\gamma$、$c_{reg}$、`MAX_DISTANCE`  
初始化探测深度 $p=1$（从最近的 1 个聚类开始）

---

### 步骤 2：逐聚类探测 + 早停判断
按**质心与查询的距离由近到远**逐次探测聚类，每探测一个聚类，合并结果并判断是否早停。

#### 伪代码
```python
def conann_search(ivf_index, x, K, best_lambda, gamma, c_reg, MAX_DISTANCE, nlist):
    p = 1
    current_topk_dists = []
    current_topk_ids = []

    while p <= nlist:
        # 1. 探测第 p 个聚类（按质心距离顺序），合并到当前结果
        cluster_result = ivf_index.search_single_cluster(x, p, K)  # 仅探测第 p 近的聚类
        current_topk_dists, current_topk_ids = merge_and_update_topk(
            current_topk_dists, current_topk_ids, cluster_result, K
        )
        # 2. 计算当前第 K 近距离 score_k
        if len(current_topk_dists) < K:
            score_k = float('inf')
        else:
            score_k = current_topk_dists[-1]  # 升序排列，最后一个是第 K 近
        # 3. 计算非一致性分数
        pi = score_k / MAX_DISTANCE
        # 4. 计算正则化非一致性分数
        reg_term = gamma * max(0, p - c_reg)
        hat_pi = (1 - pi) + reg_term
        # 5. 核心早停判断
        if hat_pi <= best_lambda:
            break  # 满足条件，早停
        # 6. 未满足，继续探测下一个聚类
        p += 1

    return current_topk_dists, current_topk_ids
```

---

## 早停逻辑关键细节
1. **探测顺序**：必须按**质心与查询的距离由近到远**探测（IVF 原生逻辑）；
2. **结果合并**：每探测一个聚类，需将新结果与之前的 top-K 合并，保持距离升序；
3. **正则化项**：仅当 $p > c_{reg}$ 时才施加惩罚（$(p - c_{reg})^+$ 取正值）；
4. **兜底逻辑**：若 $p = nlist$（探测完所有聚类），无论分数如何都强制停止。

---

# 第三部分：算法核心总结
## 离线校准（训练）
1. 强制探测所有聚类，计算非一致性分数和全局 `MAX_DISTANCE`；
2. 网格搜索调优 $\gamma, c_{reg}$；
3. 遍历候选 $\lambda$，选择满足 $\text{经验FNR} + 1/(M+1) ≤ \alpha$ 的最大 $\hat{\lambda}$。

## 在线查询（搜索）
1. 逐聚类探测，合并 top-K；
2. 计算 $\hat{\pi} = (1 - \text{score_k/MAX_DISTANCE}) + \gamma \cdot (p - c_{reg})^+$；
3. 若 $\hat{\pi} ≤ \hat{\lambda}$ → 早停，否则继续。

---

如果你愿意，我可以帮你把**上述伪代码转化为可运行的 Python 代码（基于 FAISS）**，直接对接你的 IVF 索引。