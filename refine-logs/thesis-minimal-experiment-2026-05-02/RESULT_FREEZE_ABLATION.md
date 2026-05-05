# Ablation Result Freeze

Date: 2026-05-04

## Scope

This freeze covers the thesis-minimal ablation and top-k supplement outputs:

- Triage deletion study: `triage_ablation_summary.csv`
- Dynamic scheduling study: `scheduling_ablation_summary.csv`
- Top-k supplement: `topk20_sensitivity_summary.csv`

All formal BoundFetch measurements in these summaries use the warmup-then-measurement protocol. A validation pass checked 23 BoundFetch `warm_measurement` rows and found matching warmup outputs under `outputs/ablation_warmup`.

## Triage Summary

COCO at `topk=10,nprobe=64`:

| Variant | recall@10 | e2e_ms | p95_ms | p99_ms | Run ID |
|---|---:|---:|---:|---:|---|
| full | 0.9582 | 1.2818 | 1.6449 | 1.8135 | `TRIAGE_ABLATION_coco_100k_boundfetch_canonical_triage_full_warm_measurement_1777882801773` |
| SafeIn prefetch off | 0.9582 | 1.1836 | 1.5148 | 1.6718 | `TRIAGE_ABLATION_coco_100k_boundfetch_canonical_safein_prefetch_off_warm_measurement_1777882837858` |
| Uncertain eager payload | 0.9582 | 1.3996 | 1.8417 | 2.0944 | `TRIAGE_ABLATION_coco_100k_boundfetch_canonical_uncertain_eager_warm_measurement_1777882874678` |
| SafeOut pruning off | 0.9582 | 7.4248 | 8.9294 | 9.4914 | `TRIAGE_ABLATION_coco_100k_boundfetch_canonical_safeout_off_warm_measurement_1777882918548` |

MS MARCO at `topk=10,nprobe=128`:

| Variant | recall@10 | e2e_ms | p95_ms | p99_ms | Run ID |
|---|---:|---:|---:|---:|---|
| full | 0.9320 | 9.0816 | 13.2181 | 15.5815 | `TRIAGE_ABLATION_msmarco_passage_boundfetch_canonical_triage_full_np128_warm_measurement_1777887602950` |
| SafeIn prefetch off | 0.9320 | 9.1294 | 13.3086 | 15.8043 | `TRIAGE_ABLATION_msmarco_passage_boundfetch_canonical_safein_prefetch_off_np128_warm_measurement_1777887727424` |
| Uncertain eager payload | 0.9320 | 11.0217 | 16.7117 | 19.9504 | `TRIAGE_ABLATION_msmarco_passage_boundfetch_canonical_uncertain_eager_np128_warm_measurement_1777887841951` |

MS MARCO SafeOut-off full 1000-query run is not part of the frozen formal table because the warmup did not reach the first 100-query progress point after several minutes. COCO SafeOut-off is the formal deletion evidence for SafeOut amplification.

## Scheduling Summary

COCO at `topk=10,nprobe=64`:

| Variant | recall@10 | e2e_ms | p95_ms | p99_ms | Run ID |
|---|---:|---:|---:|---:|---|
| submit_online=0 | 0.9582 | 1.3016 | 1.6633 | 1.8785 | `SCHEDULING_ABLATION_coco_100k_boundfetch_canonical_submit_online_0_warm_measurement_1777883608179` |
| submit_online=1 | 0.9582 | 1.3020 | 1.6547 | 1.8251 | `SCHEDULING_ABLATION_coco_100k_boundfetch_canonical_submit_online_1_warm_measurement_1777883644400` |
| submit_batch=4 | 0.9582 | 1.3332 | 1.7042 | 1.8992 | `SCHEDULING_ABLATION_coco_100k_boundfetch_canonical_submit_batch_4_warm_measurement_1777883681777` |
| submit_batch=8 | 0.9582 | 1.2008 | 1.5097 | 1.7286 | `SCHEDULING_ABLATION_coco_100k_boundfetch_canonical_submit_batch_8_warm_measurement_1777883718279` |
| submit_batch=16 | 0.9582 | 1.1860 | 1.5200 | 1.6887 | `SCHEDULING_ABLATION_coco_100k_boundfetch_canonical_submit_batch_16_warm_measurement_1777883754490` |
| submit_batch=32 | 0.9582 | 1.3077 | 1.6730 | 1.8347 | `SCHEDULING_ABLATION_coco_100k_boundfetch_canonical_submit_batch_32_warm_measurement_1777883790754` |

The best observed scheduling point is `submit_batch=16` by average latency and p99.

## Top-k Summary

Top20 is the frozen C4 supplement. COCO and MS MARCO both include BoundFetch, IVF+PQ+FlatStor, and IVF+RaBitQ+FlatStor.

MS MARCO top20:

| System | nprobe | recall@20 | e2e_ms | p95_ms | p99_ms | Run ID |
|---|---:|---:|---:|---:|---:|---|
| BoundFetch | 32 | 0.8339 | 8.4510 | 11.2681 | 12.4946 | `TOPK_SUPPLEMENT_msmarco_passage_boundfetch_canonical_boundfetch_topk20_np32_warm_measurement_1777884210371` |
| BoundFetch | 64 | 0.8875 | 9.7989 | 13.7359 | 15.6217 | `TOPK_SUPPLEMENT_msmarco_passage_boundfetch_canonical_boundfetch_topk20_np64_warm_measurement_1777884325993` |
| BoundFetch | 128 | 0.9278 | 11.3763 | 17.4967 | 20.0412 | `TOPK_SUPPLEMENT_msmarco_passage_boundfetch_canonical_boundfetch_topk20_np128_warm_measurement_1777884447303` |
| IVF+PQ+FlatStor | 32 | 0.6210 | 151.0294 | 191.0191 | 236.8651 | `TOPK_SUPPLEMENT_msmarco_passage_faiss_ivfpq_refine_flatstor_1777886394252` |
| IVF+PQ+FlatStor | 64 | 0.6306 | 65.8090 | 73.0038 | 99.4528 | `TOPK_SUPPLEMENT_msmarco_passage_faiss_ivfpq_refine_flatstor_1777886635106` |
| IVF+PQ+FlatStor | 128 | 0.6351 | 69.5689 | 72.6233 | 85.1618 | `TOPK_SUPPLEMENT_msmarco_passage_faiss_ivfpq_refine_flatstor_1777886789798` |
| IVF+RaBitQ+FlatStor | 32 | 0.8235 | 92.3725 | 133.1116 | 173.1076 | `TOPK_SUPPLEMENT_msmarco_passage_ivf_rabitq_rerank_flatstor_1777886948589` |
| IVF+RaBitQ+FlatStor | 64 | 0.8796 | 64.8154 | 81.6545 | 105.9600 | `TOPK_SUPPLEMENT_msmarco_passage_ivf_rabitq_rerank_flatstor_1777887118925` |
| IVF+RaBitQ+FlatStor | 128 | 0.9193 | 64.3284 | 80.5912 | 94.8355 | `TOPK_SUPPLEMENT_msmarco_passage_ivf_rabitq_rerank_flatstor_1777887248210` |

COCO top20 remains in `topk20_sensitivity_summary.csv`; BoundFetch reaches recall@20 `0.8107/0.9019/0.9529` at nprobe `16/32/64`.

## Deferred Items

- COCO top50 and top100 are supported by the benchmark path, verified with smoke runs.
- Full top50/top100 matrices are deferred as NICE appendix stress tests because smoke runs show high payload-fetch cost and top20 already provides the thesis C4 sensitivity evidence.
- Stage2 classify and boundary sensitivity remain optional appendix branches unless a later writing pass needs extra mechanism detail.
