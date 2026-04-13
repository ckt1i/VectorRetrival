# Experiment Tracker

**Date**: 2026-04-13  
**System**: BoundFetch  
**Protocol**: Warm steady-state only

| Run ID | Milestone | Purpose | System / Variant | Split | Metrics | Priority | Status | Notes |
|--------|-----------|---------|------------------|-------|---------|----------|--------|-------|
| R001 | M0 | Freeze evaluation scope | Warm-only protocol update across docs | coco_100k | protocol, reporting fields | MUST | TODO | Remove cold-start language from active plan |
| R002 | M1 | Main Pareto anchor | BoundFetch `nprobe=50` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | Existing result: 0.8018 / 0.6391ms |
| R003 | M1 | Main Pareto anchor | BoundFetch `nprobe=100` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | Existing result: 0.8748 / 0.9323ms |
| R004 | M1 | Main Pareto anchor | BoundFetch `nprobe=200` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | Existing result: 0.8984 / 1.1414ms |
| R005 | M1 | Main Pareto anchor | BoundFetch `nprobe=300` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | Existing result: 0.8988 / 1.1530ms |
| R006 | M1 | Main Pareto anchor | BoundFetch `nprobe=500` | coco_100k | recall@10, e2e_ms, p99_ms | MUST | DONE | Existing result: 0.8988 / 1.1595ms |
| R007 | M1 | Strengthen Pareto density | BoundFetch intermediate point(s) such as `nprobe=150/250` if available | coco_100k | recall@10, e2e_ms, p99_ms | MUST | TODO | Only add if needed to clarify the curve |
| R008 | M1 | Baseline anchor | DiskANN+FlatStor strongest warm point | coco_100k | recall@10, e2e_ms | MUST | DONE | Existing result: 1.000 / 3.7899ms |
| R009 | M1 | Baseline anchor | FAISS-IVFPQ-disk+FlatStor strongest warm point | coco_100k | recall@10, e2e_ms | MUST | DONE | Existing result: 0.747 / 1.3124ms |
| R010 | M2 | Mechanism attribution | BoundFetch best point breakdown | coco_100k | uring_submit_ms, probe_ms, io_wait_ms, submit_calls | MUST | DONE | Existing result at nprobe=200 already available |
| R011 | M2 | Stability check | BoundFetch nearby-point breakdown | coco_100k | same as R010 | MUST | TODO | Use `nprobe=100` or `300` for contrast |
| R012 | M3 | Recall improvement | Tune `CRC_alpha` low side | coco_100k | recall@10, e2e_ms, submit_calls | MUST | TODO | Keep `io_qd=64`, `mode=shared` fixed |
| R013 | M3 | Recall improvement | Tune `CRC_alpha` high side | coco_100k | recall@10, e2e_ms, submit_calls | MUST | TODO | Single-factor change only |
| R014 | M3 | Recall improvement | Relax pruning / verification threshold | coco_100k | recall@10, e2e_ms, probe_ms | MUST | TODO | Purpose: test whether recall can move without large latency loss |
| R015 | M3 | Simplification check | Current best config vs simplified pruning policy | coco_100k | recall@10, e2e_ms | MUST | TODO | Decide whether some mechanism can be deleted |
| R016 | M4 | Generality support | BoundFetch representative point on Deep1M | deep1m | recall, latency, breakdown | NICE | TODO | Only after M1-M3 succeed |
| R017 | M4 | Generality support | BoundFetch representative point on Deep8M | deep8m | recall, latency, breakdown | NICE | TODO | Skip if it delays writeup |
| R018 | M4 | Freeze appendix checks | qd sweep summary | coco_100k | e2e_ms | NICE | DONE | Existing qd=64/128/256/512 results available |
| R019 | M4 | Freeze appendix checks | shared vs isolated summary | coco_100k | e2e_ms, submit_calls | NICE | DONE | Existing result available |
| R020 | M4 | Freeze appendix checks | SQPOLL fallback summary | coco_100k | e2e_ms, effective flag | NICE | DONE | Existing `effective=0` result available |
