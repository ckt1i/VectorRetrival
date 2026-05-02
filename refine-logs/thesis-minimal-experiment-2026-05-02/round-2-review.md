# Round 2 Review

<details>
<summary>Raw reviewer response</summary>

这版已经到“可以按本科毕业论文节奏快速推进”的门槛了。上一轮的主要结构性问题基本都被修掉了：问题锚点更稳，方法协议已经成型，主实验矩阵也终于压到了一个本科论文能落地的规模。

| 维度 | 分数 | 简评 |
|---|---:|---|
| Problem Fidelity | 9 | 锚点稳定，没有再往 CRC/RAIR/大 benchmark 漂。 |
| Method Specificity | 8 | `top-C` protocol 已明确，但 matched-recall 选点规则还要再写死。 |
| Contribution Quality | 8 | 主贡献/支撑贡献分层清楚，适合本科论文定位。 |
| Frontier Leverage | 8 | `retrieve -> fetch -> rerank` 的 framing 是当下系统问题，且不过度追热点。 |
| Feasibility | 8 | 两周推进路径基本可信，前提是 COCO 主闭环和 `C`/serial-fetch 开关可落地。 |
| Validation Focus | 9 | Mandatory / Second / Appendix 分层合理，主线足够小。 |
| Venue/Thesis Readiness | 8 | 已经有比较像 VLDB/PVLDB 系统论文的骨架。 |

**OVERALL SCORE: 8.2 / 10**

**Verdict:** `READY`

**Drift Warning:** `NONE`  
前提是你严格维持现在的分层，不把 `Lance`、`msmarco_passage`、更多 payload 档位重新拉回主线成功条件。

**剩余 blocking issues**
1. `CRITICAL`：先冻结 matched-recall 选点规则。  
建议写死成一句话：主表对每个系统取“达到共同 recall target `R*` 的最低 E2E latency”，`R*` 由所有系统都能达到的最高共同 recall 决定。

2. `CRITICAL`：确认当前 binary/runner 能稳定控制 top-`C` 或等价 rerank budget。  
如果没有这个旋钮，你的固定 `top-C` protocol 还只是叙事，不是可执行方法。

3. `IMPORTANT`：`overlap off / serial fetch` 必须是干净 ablation。  
除了关闭重叠，不应连带改变候选规模、缓存策略、submission mode 或其他路径。

4. `IMPORTANT`：给 `deep1m_synth` 准备 fallback。  
如果生成拖期，就退化为 1 个 mandatory payload size，必要时用 COCO 上的 synthetic payload padding 做 sensitivity，别让扩展实验卡主线。

5. `IMPORTANT`：warm-serving/cache protocol 先写清。  
包括 warmup 次数、是否丢首轮、缓存是否共享、baseline 与 BoundFetch 是否同口径。

结论很直接：这版已经不是“还要继续想”的 proposal，而是可以直接进入实现和跑数阶段的 proposal。现在该做的是先把上面几个执行口径钉死，然后用 3 天打穿 `COCO100K` 主闭环。

</details>
