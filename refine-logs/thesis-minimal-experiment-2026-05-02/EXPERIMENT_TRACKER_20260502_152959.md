# Experiment Tracker

| Run ID | Milestone | Purpose | Dataset | System / Variant | Backend | Key Params | Metrics | Priority | Status | Notes |
|--------|-----------|---------|---------|------------------|---------|------------|---------|----------|--------|-------|
| T000 | M0 | asset check | coco_100k | dataset/GT/backend readiness | all | gt_top100, FlatStor, Lance | readiness | MUST | TODO | confirm current formal paths |
| T001 | M0 | asset check | msmarco_passage | dataset/GT/qrels/backend readiness | all | gt_top100, qrels, FlatStor, Lance | readiness | MUST | TODO | qrels may be delayed |
| T002 | M1 | sanity | coco_100k | BoundFetch-Guarded | integrated | nlist=2048,nprobe=64,topk=10,crc=0,bits=4 | recall@10,e2e,p99,triage | MUST | TODO | first in-repo run |
| T003 | M1 | sanity | coco_100k | IVF+PQ | FlatStor | nlist=2048,nprobe=64,topk=10,candidate_budget=100,bits=4 | recall@10,e2e,p99 | MUST | TODO | baseline path smoke test |
| T004 | M1 | sanity | coco_100k | IVF+RaBitQ | FlatStor | nlist=2048,nprobe=64,topk=10,candidate_budget=100,bits=4 | recall@10,e2e,p99 | MUST | TODO | official RaBitQ baseline smoke test |
| T005 | M2 | main sweep | coco_100k | BoundFetch-Guarded | integrated | nprobe=16,32,64,128,256,512;topk=10 | recall@10,e2e,p95,p99,triage | MUST | TODO | use warm full_preload |
| T006 | M2 | main sweep | coco_100k | IVF+PQ | FlatStor | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | MUST | TODO | formal baseline |
| T007 | M2 | main sweep | coco_100k | IVF+RaBitQ | FlatStor | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | MUST | TODO | formal baseline |
| T008 | M2 | main sweep | coco_100k | IVF+PQ | Lance | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | SHOULD | TODO | move appendix if blocked |
| T009 | M2 | main sweep | coco_100k | IVF+RaBitQ | Lance | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | SHOULD | TODO | move appendix if blocked |
| T010 | M3 | sanity | msmarco_passage | BoundFetch-Guarded | integrated | nlist=16384,nprobe=128,topk=10,crc=0,bits=4 | recall@10,e2e,p99,triage | MUST | TODO | verify data scale |
| T011 | M3 | main sweep | msmarco_passage | BoundFetch-Guarded | integrated | nprobe=16,32,64,128,256,512,1024;topk=10 | recall@10/MRR@10,e2e,p95,p99 | MUST | TODO | qrels optional first pass |
| T012 | M3 | main sweep | msmarco_passage | IVF+PQ | FlatStor | nprobe=16,32,64,128,256,512,1024;topk=10;budget=100 | recall@10/MRR@10,e2e,p95,p99 | MUST | TODO | formal baseline |
| T013 | M3 | main sweep | msmarco_passage | IVF+RaBitQ | FlatStor | nprobe=16,32,64,128,256,512,1024;topk=10;budget=100 | recall@10/MRR@10,e2e,p95,p99 | MUST | TODO | formal baseline |
| T014 | M3 | main sweep | msmarco_passage | IVF+PQ | Lance | nprobe=16,32,64,128,256,512,1024;topk=10;budget=100 | recall@10/MRR@10,e2e,p95,p99 | SHOULD | TODO | after FlatStor |
| T015 | M3 | main sweep | msmarco_passage | IVF+RaBitQ | Lance | nprobe=16,32,64,128,256,512,1024;topk=10;budget=100 | recall@10/MRR@10,e2e,p95,p99 | SHOULD | TODO | after FlatStor |
| T016 | M4 | ablation | coco_100k | SafeOut pruning off | FlatStor | matched nprobe,topk=10 | reranked,bytes,e2e,p99 | MUST | TODO | may require flag |
| T017 | M4 | ablation | coco_100k | SafeIn prefetch off | FlatStor | matched nprobe,topk=10 | payload_prefetch,missing_fetch,e2e | MUST | TODO | proxy via threshold if no flag |
| T018 | M4 | ablation | coco_100k | Uncertain eager payload | FlatStor | matched nprobe,topk=10 | bytes,payload_reads,e2e | MUST | TODO | may require flag |
| T019 | M4 | ablation | coco_100k | Stage2 classify off/on | FlatStor | stage2 flags,matched nprobe | s2_safe_out,reranked,e2e | SHOULD | TODO | use existing stage2 flags if valid |
| T020 | M4 | ablation | coco_100k | Boundary sensitivity | FlatStor | override-eps/dk or epsilon percentile | triage ratio,recall,e2e | SHOULD | TODO | keep small sweep |
| T021 | M5 | scheduling | coco_100k | submit-online on/off | FlatStor | matched nprobe,topk=10 | p95,p99,io_wait,submit_calls | MUST | TODO | scheduler claim |
| T022 | M5 | scheduling | coco_100k | submit-batch sweep | FlatStor | submit_batch=4,8,16,32 | p95,p99,flushes | MUST | TODO | default 8 |
| T023 | M5 | scheduling | coco_100k | safein_all_threshold sweep | FlatStor | low/default/off | queue pressure,bytes,e2e | SHOULD | TODO | prefetch pressure |
| T024 | M6 | top-k sensitivity | coco_100k | BoundFetch + FlatStor baselines | FlatStor | topk=20,budget=150/200 | recall@20,e2e,p99 | SHOULD | TODO | requires recall@20 support |
| T025 | M6 | stress | coco_100k | BoundFetch + FlatStor baselines | FlatStor | topk=50,budget=250 | recall@50,e2e,p99 | NICE | TODO | appendix only |
| T026 | M6 | payload sensitivity | deep1m_synth/deep8m_synth | BoundFetch + FlatStor baselines | FlatStor | payload=256B,4KB,64KB | bytes,e2e,p99 | NICE | TODO | use deep8m fallback if needed |
| T027 | M7 | reporting | all | matched-quality selection | all | Q* per dataset | final tables | MUST | TODO | no cherry-picking |
