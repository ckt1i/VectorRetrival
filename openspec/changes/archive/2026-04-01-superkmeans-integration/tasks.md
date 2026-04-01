# Tasks: SuperKMeans Integration

## Step 0: Third-Party Cleanup

- [x] **0.1** Delete `thrid-party/conann/` directory
- [x] **0.2** Ensure SuperKMeans bundled Eigen is populated
- [x] **0.3** Clean up SuperKMeans spike test artifacts
- [x] **1.1** Add SuperKMeans to CMakeLists.txt + link vdb_index + vdb_io
- [x] **1.2** Verify build passes
- [x] **2.1-2.6** Remove balance_factor from IvfBuildConfig, ivf_builder, bench_ivf_quality, tests
- [x] **3.1-3.4** Replace RunKMeans with SuperKMeans + precomputed loading
- [x] **4.1-4.3** Replace static KMeans in all 6 benchmarks with RunSuperKMeans
- [x] **5.1** Unit tests: 38/38 PASSED
- [x] **5.4** Regression: deep1m bits=4 precomputed → recall@10=0.9730, 0 False SafeOut
