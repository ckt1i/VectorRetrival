## 1. Protocol And Documentation Alignment

- [x] 1.1 Rewrite the active experiment narrative to make warm steady-state the only required protocol
  - **Status**: Done — FINAL_PROPOSAL.md and proposal.md already establish warm-only as the only required protocol
- [x] 1.2 Remove cold-start and drop-cache work from the main-path next-step plan, and mark them as non-goals
  - **Status**: Done — Non-goals explicitly stated in FINAL_PROPOSAL.md line 10 and design.md Goals/Non-Goals section
- [x] 1.3 Align English and Chinese planning documents around the same warm-only thesis
  - **Status**: Done — FINAL_PROPOSAL.md and FINAL_PROPOSAL_CN.md both state warm-only thesis
- [x] 1.4 Add runbook-level guidance to the proposal/design so each required experiment block has fixed settings, variable settings, outputs, and stop conditions
  - **Status**: Done — design.md Blocks A/B/C/D contain runbook-level guidance with fixed settings, variable settings, outputs, and stop conditions

## 2. Main Warm Pareto Block

- [x] 2.1 Fix the main block settings to `coco_100k`, `queries=1000`, `topk=10`, `bits=4`, `io_qd=64`, `mode=shared`
  - **Status**: Done — design.md Block A "固定设置" specifies all these parameters
- [x] 2.2 Freeze the BoundFetch skeleton sweep to `nprobe={200,500}` with `crc-alpha={0.01,0.02,0.05,0.08,0.1,0.15,0.2}`
  - **Status**: Done — design.md Block A now locks the primary BoundFetch sweep to the exact grid requested by the user
- [x] 2.3 Freeze the follow-up validation sweep to `epsilon-percentile={0.99,0.95,0.9}` and `nlist={1024,2048}` with pruning rules
  - **Status**: Done — design.md Block A "BoundFetch 执行裁剪规则" defines the exact epsilon and nlist validation sequence without exploding into a full cartesian product
- [x] 2.4 Freeze the two baseline families for the main E2E figure and table
  - **Status**: Done — design.md Block A "Baseline 参数矩阵" freezes DiskANN-disk+FlatStor-sim and FAISS-IVFPQ-disk+FlatStor-sim
- [x] 2.5 Define executable baseline parameter sweeps that will produce two additional Pareto curves
  - **Status**: Done — design.md Block A fixes DiskANN `L_search={32,64,96,128,160,192,256}` and FAISS `nprobe={8,16,32,64,128,256,512}`
- [x] 2.6 Specify the required reporting fields for `recall@10`, `e2e_ms`, `p99_ms`, and key mechanism counters
  - **Status**: Done — design.md Block A "输出要求" specifies all fields: system, dataset, topk, param, recall@10, vec_search_ms, payload_ms, e2e_ms, notes, protocol
- [x] 2.7 Specify that results must be written to `baselines/results/e2e_comparison_warm.csv` and summarized in `baselines/results/analysis.md`
  - **Status**: Done — design.md Block A "输出要求" specifies these output paths
- [x] 2.8 Add a handoff-ready execution table that another runner can follow without re-interpreting the plan
  - **Status**: Done — design.md adds "可执行实验表" and "Handoff Table" with ordered batches, goals, outputs, and backfill steps

## 3. Mechanism Attribution And Recall Ablation

- [x] 3.1 Define the mechanism attribution block for the best warm operating point and one nearby contrast point
  - **Status**: Done — design.md Block B now fixes the main point to `nprobe=200, crc-alpha=0.05, epsilon=0.99, nlist=2048` and requires contrast from neighboring alpha / higher nprobe candidates
- [x] 3.2 List the exact required breakdown fields for attribution output
  - **Status**: Done — design.md Block B "输出要求" lists: uring_submit_ms, probe_ms, parse_cluster_ms, rerank_ms, io_wait_ms, submit_calls
- [x] 3.3 Define the minimal recall-improvement ablation over `CRC_alpha`, `epsilon-percentile`, `nlist`, pruning threshold, or verification policy
  - **Status**: Done — design.md Block C "参数范围" defines the ablation variable set
- [x] 3.4 Fix the recall-ablation control variables so only one mechanism family changes at a time
  - **Status**: Done — design.md Block C "固定设置" specifies io_qd=64, mode=shared, and "每次只改变一个机制族"
- [x] 3.5 Add an explicit optimization gate: continue code tuning only if recall improves at near-current latency
  - **Status**: Done — design.md Block C explicitly says baseline Pareto alignment comes before any additional CPU/IO co-optimization decision
- [x] 3.6 Add an explicit stop condition for freezing further optimization if no acceptable recall gain appears
  - **Status**: Done — design.md Block C "停止条件" specifies: if no variant achieves acceptable recall gain, freeze code optimization and move to writeup

## 4. Execution Order And Tracker Integration

- [x] 4.1 Convert the new warm-serving plan into an ordered milestone list with stop/go gates
  - **Status**: Done — design.md "Handoff Table" and "Migration Plan" define the ordered execution blocks and the go/no-go decision point after three-curve Pareto completion
- [x] 4.2 Update the tracker with run IDs, priorities, and which results already exist
  - **Status**: Done — EXPERIMENT_TRACKER.md already contains run IDs R001-R020 with milestone mapping, priorities (MUST/NICE), and status (DONE/TODO) for each
- [x] 4.3 Separate must-run main-paper experiments from appendix-only validation checks
  - **Status**: Done — EXPERIMENT_TRACKER.md separates MUST (M1-M3) from NICE (M4). design.md Block D explicitly freezes qd/mode/SQPOLL as appendix
- [x] 4.4 Add required document backfill steps after each block (`analysis.md`, aggregate CSV, tracker docs)
  - **Status**: Done — design.md "Migration Plan" step 4 specifies: after each block, backfill to e2e_comparison_warm.csv, analysis.md, and EXPERIMENT_TRACKER*.md
- [x] 4.5 Freeze qd / mode / SQPOLL checks as appendix-only validation unless the runtime environment changes
  - **Status**: Done — design.md Block D "附录冻结项" and FINAL_PROPOSAL.md explicitly freeze these as appendix supporting evidence
