# epsilon=0.75 Anchor CPU Profiling

## Target

- Dataset: `coco_100k`
- Index: `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.75`
- Query config: `nlist=2048`, `nprobe=200`, `crc-alpha=0.02`, `bits=4`, `clu_mode=full_preload`
- Main anchor metrics: `recall@10=0.9519`, `avg_query=1.786ms`, `p99=2.088ms`

## Artifacts

- Command: `profile_output/epsilon075_anchor/command.txt`
- Baseline wall-time / RSS: `profile_output/epsilon075_anchor/baseline.time`
- perf stat: `profile_output/epsilon075_anchor/perf_stat.txt`
- perf data: `profile_output/epsilon075_anchor/perf.data`
- perf report: `profile_output/epsilon075_anchor/perf_report.txt`

## Key Observations

1. Whole-benchmark `perf` is dominated by benchmark scaffolding:
   - top sample is brute-force GT `vdb::simd::L2Sqr(...)`
   - this is expected because `bench_e2e` always runs GT + CRC calibration before query serving
2. Query hot path is still CPU-bound:
   - benchmark reports `io_wait=0.000ms`, `cpu=1.786ms`, `probe=1.500ms`, `uring_submit=0.089ms`, `rerank_cpu=0.012ms`
   - `.clu` full preload has already removed cluster-side I/O from the steady-state bottleneck
3. Within the query path, the biggest remaining hotspots are per-query preparation:
   - `QuantizeQuery14Bit(...)`
   - `PrepareQueryRotatedInto(...)`
   - `EstimateDistanceFastScan(...)` is present but materially smaller

## Rough Query-Side Headroom

Using the query-only timing (`1.786ms/query`) plus the sampled query hotspots:

- `QuantizeQuery14Bit`: about `0.302ms/query` (`16.9%`)
- `PrepareQueryRotatedInto`: about `0.296ms/query` (`16.6%`)
- Combined query preparation: about `0.598ms/query` (`33.5%`)
- `EstimateDistanceFastScan`: about `0.079ms/query` (`4.4%`)
- `IPExRaBitQ`: about `0.109ms/query` (`6.1%`)
- `uring_submit`: about `0.089ms/query` (`5.0%`)

Interpretation:

- If query-preparation cost is reduced by `30%`, the expected gain is about `0.18ms/query`
- If query-preparation cost is reduced by `50%`, the expected gain is about `0.30ms/query`
- A very aggressive near-elimination of current preparation overhead would save at most about `0.60ms/query`

This means CPU optimization can still matter, but the realistic near-term gain is more likely in the `0.15-0.30ms` range than another large class of `0.8ms+` improvement.

## Recommended Next Pass

1. Fuse or cache query-side preprocessing to reduce repeated `QuantizeQuery14Bit` / rotated-query setup.
2. Inspect whether `PreparedQuery` generation can be hoisted or reused across cluster probing.
3. Only after that, consider micro-optimizing FastScan / Stage 2 kernels.
