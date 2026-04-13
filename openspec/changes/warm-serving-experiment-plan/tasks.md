## 1. Proposal And Protocol Alignment

- [ ] 1.1 Rewrite the active experiment narrative to make warm steady-state the only required protocol
- [ ] 1.2 Remove cold-start and drop-cache work from the main-path next-step plan, and mark them as non-goals
- [ ] 1.3 Align English and Chinese planning documents around the same warm-only thesis

## 2. Main Warm Result Definition

- [ ] 2.1 Define the main COCO 100K warm Pareto block, including the required BoundFetch operating points
- [ ] 2.2 Freeze the baseline comparison set for the main E2E figure and table
- [ ] 2.3 Specify the required reporting fields for `recall@10`, `e2e_ms`, `p99_ms`, and key mechanism counters

## 3. Minimal Ablation And Decision Gates

- [ ] 3.1 Define the mechanism attribution block for the best warm operating point and one nearby contrast point
- [ ] 3.2 Define the minimal recall-improvement ablation over `CRC_alpha`, pruning threshold, or verification policy
- [ ] 3.3 Add an explicit optimization gate: continue code tuning only if recall improves at near-current latency

## 4. Execution Plan And Tracking

- [ ] 4.1 Convert the new warm-serving plan into an ordered milestone list with stop/go gates
- [ ] 4.2 Update the tracker with run IDs, priorities, and which results already exist
- [ ] 4.3 Separate must-run main-paper experiments from appendix-only validation checks
