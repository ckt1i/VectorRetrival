# Pipeline Summary: Thesis Minimal Experiment Post-Main-Sweep Update

**Problem**: 在主实验已经完成的情况下，将后续实验计划从“扩大主矩阵”正式切换为“结果固化 + 机制消融 + top-k 补充”。  
**Final Method Thesis**: BoundFetch-Guarded 通过 RaBitQ bound-guided SafeIn/SafeOut/Uncertain 三分类和动态异步读取调度，降低 payload-rich vector retrieval 的 matched-quality E2E latency。  
**Final Verdict**: READY for ablation and paper-result consolidation  
**Date**: 2026-05-03

## Final Deliverables

- Proposal: `refine-logs/thesis-minimal-experiment-2026-05-02/FINAL_PROPOSAL.md`
- Experiment plan: `refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_PLAN.md`
- Experiment tracker: `refine-logs/thesis-minimal-experiment-2026-05-02/EXPERIMENT_TRACKER.md`
- Post-main-sweep refinement: `refine-logs/thesis-minimal-experiment-2026-05-02/POST_MAIN_SWEEP_REFINEMENT.md`

## Contribution Snapshot

- Dominant contribution: bound-guided selective fetch，将候选分类直接映射为不同 vector/payload 读取策略。
- Supporting contribution: dynamic async scheduling，用小规模调度消融验证其对 p95/p99 的贡献。
- Explicitly rejected expansion: 不新增 baseline family，不把 synthetic payload sensitivity 放入当前本科论文主线。

## Current Main Result

- COCO100K 和 MS MARCO 的 T000-T015 主实验已完成。
- 主实验已经支撑 RQ1：BoundFetch-Guarded 在 retained result set 中快于四组 baseline。
- 后续实验不再扩大主实验矩阵。

## Must-Prove Claims After Main Sweep

- RQ2: SafeIn/SafeOut/Uncertain 的机制收益必须通过 deletion ablation 支撑。
- RQ3: dynamic async scheduling 的 tail-latency 收益必须通过 submit-online / submit-batch 消融支撑。
- RQ4: 更大 top-k 下的趋势至少用 top20 补充验证；top50/top100 只作为 appendix stress。

## Next Runs To Launch

1. T029: 固化 matched-quality Q*、曲线点和论文主表代表点。
2. T016-T018: SafeOut off、SafeIn prefetch off、Uncertain eager payload。
3. T021-T022: submit-online on/off 与 submit-batch sweep。

## Main Risks

- MS MARCO nprobe=256 当前 p99 异常，需要 T028 复核或在论文中标注 noisy point。
- 三分类消融可能需要补最小运行开关；若 proxy 参数不能隔离机制，应优先加最小 flag。
- top20/top50/top100 需要 recall 输出或后处理支持，不能提前写结论。

## Next Action

Proceed to the post-main-sweep ablation runs in `EXPERIMENT_TRACKER.md`.
