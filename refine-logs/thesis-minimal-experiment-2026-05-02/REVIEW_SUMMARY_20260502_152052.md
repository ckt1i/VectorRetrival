# Review Summary

**Problem**: 为本科毕业论文收敛 VectorRetrival/BoundFetch 的最小完整实验方案。  
**Date**: 2026-05-02  
**Rounds**: 2  
**Final Score**: 8.2 / 10  
**Final Verdict**: AUTHOR-REVISED after user supplement; ready for experiment planning, not externally re-reviewed

## Problem Anchor

在本科毕业论文时间约束下，缩小到单聚类划分、无 CRC 早停、有限数据集、有限 baseline 的主实验与 ablation，证明 integrated vector retrieval + payload fetch serving path 的 E2E 收益。

## Round-by-Round Resolution Log

| Round | Main Reviewer Concerns | What Changed | Solved? | Remaining Risk |
|-------|-------------------------|--------------|---------|----------------|
| 1 | 方法像组件列表；3 数据集、4 baseline、Lance/MS MARCO 仍过宽 | 改成 top-C protocol；主线压成 COCO100K + FlatStor + 两搜索核；deep1m_synth 为扩展 | Yes | 需要确认 top-C 控制旋钮 |
| 2 | matched-recall、top-C、cache、overlap ablation 需要钉死 | 最终 proposal 写入选点规则、clean ablation、cache protocol、fallback | Yes | 执行阶段需验证 binary 参数 |
| 3 | User argued COCO-only lacks generality and fixed top-C misrepresents implementation | Restored COCO100K + MS MARCO as mandatory; changed method to RaBitQ bound-guided SafeIn/SafeOut/Uncertain triage plus dynamic io_uring scheduling | Yes | Needs execution validation on MS MARCO and Lance |
| 4 | User asked about outline placement, larger top-k, and ablation depth | Moved workload differences to experiment setup; removed CRC/RAIR boundary from method chapter; added top20/top50 policy and deeper triage/scheduling ablations | Yes | `recall@20/@50` requires benchmark output extension or post-processing |

## Overall Evolution

- 从“缩小版 formal benchmark study”收敛为一个系统 query plan。
- Round 3 corrected the scope: COCO100K-only is too narrow for a solid thesis claim; MS MARCO is now mandatory and Deep1M_synth is a strong target.
- Round 3 corrected the method: fixed top-C is replaced by RaBitQ error-bound SafeIn/SafeOut/Uncertain selective fetch.
- Round 4 corrected the thesis outline and expanded evaluation depth without expanding baseline families.

## Final Status

- Anchor status: preserved
- Focus status: broader than Round 2 but more academically defensible
- Modernity status: appropriately framed as modern embedding retrieval serving
- Strongest parts: bound-guided triage、dynamic fetch scheduling、COCO+MS MARCO multi-workload validation、top-k sensitivity、triage/scheduling ablation hierarchy
- Remaining weaknesses: `msmarco_passage` 数据与 qrels/ground truth 需要确认；Lance runner 需要执行验证；Deep1M_synth 仍需生成或 fallback；`recall@20/@50` 需要扩展输出或后处理。
