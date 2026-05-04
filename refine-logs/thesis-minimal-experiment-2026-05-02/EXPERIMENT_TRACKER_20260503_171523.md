# Experiment Tracker

| Run ID | Milestone | Purpose | Dataset | System / Variant | Backend | Key Params | Metrics | Priority | Status | Notes |
|--------|-----------|---------|---------|------------------|---------|------------|---------|----------|--------|-------|
| T000 | M0 | asset check | coco_100k | dataset/GT/backend readiness | all | gt_top100, FlatStor, Lance | readiness | MUST | DONE | canonical COCO assets and reuse chain verified |
| T001 | M0 | asset check | msmarco_passage | dataset/GT/backend readiness | all | gt_top100, FlatStor, Lance | readiness | MUST | DONE | embeddings/gt_top100/FlatStor/Lance all present; thesis metrics use ANN recall |
| T002 | M1 | sanity | coco_100k | BoundFetch-Guarded | integrated | nlist=2048,nprobe=64,topk=10,crc=1,clu_read_mode=full_preload,use_resident_clusters=1,early_stop=0,skip_gt=0,bits=4 | recall@10,e2e,p99,triage | MUST | DONE | serial rerun completed; measurement-only sanity; backend=integrated; points=64 |
| T003 | M1 | sanity | coco_100k | IVF+PQ | FlatStor | nlist=2048,nprobe=64,topk=10,candidate_budget=100,bits=4 | recall@10,e2e,p99 | MUST | DONE | serial rerun completed; backend=flatstor; points=64 |
| T004 | M1 | sanity | coco_100k | IVF+RaBitQ | FlatStor | nlist=2048,nprobe=64,topk=10,candidate_budget=100,bits=4 | recall@10,e2e,p99 | MUST | DONE | canonical reuse smoke: recall@10=0.8333, e2e=1.7916 ms, p99=2.9119 ms |
| T005 | M2 | main sweep | coco_100k | BoundFetch-Guarded | integrated | nprobe=16,32,64,128,256,512;topk=10 | recall@10,e2e,p95,p99,triage | MUST | DONE | serial rerun completed with warmup-then-measurement; backend=integrated; points=16,32,64,128,256,512 |
| T006 | M2 | main sweep | coco_100k | IVF+PQ | FlatStor | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | MUST | DONE | serial rerun completed; backend=flatstor; points=16,32,64,128,256,512 |
| T007 | M2 | main sweep | coco_100k | IVF+RaBitQ | FlatStor | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | MUST | DONE | serial rerun completed; backend=flatstor; points=16,32,64,128,256,512 |
| T008 | M2 | main sweep | coco_100k | IVF+PQ | Lance | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | SHOULD | DONE | serial rerun completed; backend=lance; points=16,32,64,128,256,512 |
| T009 | M2 | main sweep | coco_100k | IVF+RaBitQ | Lance | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | SHOULD | DONE | serial rerun completed; backend=lance; points=16,32,64,128,256,512 |
| T010 | M3 | sanity | msmarco_passage | BoundFetch-Guarded | integrated | nlist=16384,nprobe=128,topk=10,crc=1,early_stop=0,skip_gt=0,bits=4 | recall@10,e2e,p99,triage | MUST | DONE | serial rerun completed; measurement-only sanity; backend=integrated; points=128 |
| T011 | M3 | main sweep | msmarco_passage | BoundFetch-Guarded | integrated | nprobe=16,32,64,128,256,512,1024;topk=10 | recall@10,e2e,p95,p99 | MUST | DONE | serial rerun completed with warmup-then-measurement; backend=integrated; points=16,32,64,128,256,512,1024; nprobe=256 p99 is noisy |
| T012 | M3 | main sweep | msmarco_passage | IVF+PQ | FlatStor | nprobe=16,32,64,128,256,512,1024;topk=10;budget=100 | recall@10,e2e,p95,p99 | MUST | DONE | serial rerun completed; backend=flatstor; points=16,32,64,128,256,512,1024 |
| T013 | M3 | main sweep | msmarco_passage | IVF+RaBitQ | FlatStor | nprobe=16,32,64,128,256,512,1024;topk=10;budget=100 | recall@10,e2e,p95,p99 | MUST | DONE | serial rerun completed; backend=flatstor; points=16,32,64,128,256,512,1024 |
| T014 | M3 | main sweep | msmarco_passage | IVF+PQ | Lance | nprobe=16,32,64,128,256,512,1024;topk=10;budget=100 | recall@10,e2e,p95,p99 | SHOULD | DONE | serial rerun completed; backend=lance; points=16,32,64,128,256,512,1024 |
| T015 | M3 | main sweep | msmarco_passage | IVF+RaBitQ | Lance | nprobe=16,32,64,128,256,512,1024;topk=10;budget=100 | recall@10,e2e,p95,p99 | SHOULD | DONE | serial rerun completed; backend=lance; points=16,32,64,128,256,512,1024 |
| T016 | M5 | ablation | coco_100k | SafeOut pruning off | FlatStor | nprobe=64/128,topk=10 | recall@10,reranked,bytes,e2e,p95,p99,triage | MUST | TODO | proves SafeOut pruning reduces rerank/read amplification |
| T017 | M5 | ablation | coco_100k | SafeIn prefetch off | FlatStor | nprobe=64/128,topk=10 | recall@10,payload_prefetch,missing_fetch,e2e,p95,p99 | MUST | TODO | proves SafeIn payload prefetch reduces materialization wait |
| T018 | M5 | ablation | coco_100k | Uncertain eager payload | FlatStor | nprobe=64/128,topk=10 | recall@10,bytes,payload_reads,e2e,p95,p99 | MUST | TODO | proves delayed payload avoids wasted reads |
| T019 | M5 | ablation | coco_100k | Stage2 classify off/on | FlatStor | stage2 flags,nprobe=64/128 | s2_safe_out,reranked,e2e | SHOULD | TODO | appendix only if existing flags isolate it cleanly |
| T020 | M5 | ablation | coco_100k | Boundary sensitivity | FlatStor | override-eps/dk or epsilon percentile,nprobe=64/128 | triage ratio,recall,e2e | SHOULD | TODO | keep small sweep |
| T021 | M6 | scheduling | coco_100k | submit-online on/off | FlatStor | nprobe=64/128,topk=10 | p95,p99,io_wait,submit_calls | MUST | TODO | validates dynamic online submit policy |
| T022 | M6 | scheduling | coco_100k | submit-batch sweep | FlatStor | submit_batch=4,8,16,32;nprobe=64/128 | p95,p99,submit_calls,flushes | MUST | TODO | validates batch-size latency/overhead tradeoff |
| T023 | M6 | scheduling | coco_100k | safein_all_threshold sweep | FlatStor | disabled/default/large;nprobe=64/128 | queue pressure,bytes,e2e,p99 | SHOULD | TODO | tests SafeIn payload prefetch pressure |
| T024 | M7 | top-k sensitivity | coco_100k | BoundFetch + FlatStor baselines | FlatStor | topk=20,budget=150/200 | recall@20,e2e,p99 | SHOULD | TODO | requires recall@20 support |
| T025 | M7 | stress | coco_100k | BoundFetch + FlatStor baselines | FlatStor | topk=50,budget=250 | recall@50,e2e,p99 | NICE | TODO | appendix only |
| T026 | M7 | payload sensitivity | deep1m_synth/deep8m_synth | BoundFetch + FlatStor baselines | FlatStor | payload=256B,4KB,64KB | bytes,e2e,p99 | NICE | OUT-OF-SCOPE | current thesis blueprint excludes synthetic payload sensitivity |
| T027 | M8 | reporting | all | matched-quality selection | all | Q* per dataset | final tables | MUST | TODO | no cherry-picking |
| T028 | M4 | result hygiene | msmarco_passage | BoundFetch-Guarded noisy point rerun | integrated | nprobe=256,topk=10 | recall@10,e2e,p95,p99 | SHOULD | TODO | rerun or mark current p99 outlier before tail-latency claims |
| T029 | M4 | result hygiene | all | matched-quality finalization | all | retained main sweep | Q*,curve points,table rows | MUST | TODO | freeze Q* and representative points before ablations |
| T030 | M7 | stress | coco_100k | BoundFetch + FlatStor baselines | FlatStor | topk=100,budget=500 | recall@100,e2e,p99 | NICE | TODO | appendix only; requires recall@100 support |
