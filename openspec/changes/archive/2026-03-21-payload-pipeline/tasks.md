# Tasks: Payload Pipeline + Npy Loader

## Task 1: FlatBuffers schema -- add PayloadColumnSchema
- [x] Add `PayloadColumnSchema` table to `schema/segment_meta.fbs` (fields: id, name, dtype, nullable)
- [x] Add `payload_schemas: [PayloadColumnSchema]` field to `SegmentMeta` table
- [x] Regenerate `segment_meta_generated.h` via flatc
- [x] Verify existing tests still compile (no breaking change)

**Files**: `schema/segment_meta.fbs`, `CMakeLists.txt` (if flatc invocation changes)

## Task 2: IvfBuilder -- PayloadFn callback
- [x] Define `PayloadFn` type alias in `include/vdb/index/ivf_builder.h`
- [x] Add `PayloadFn payload_fn = nullptr` parameter to `Build()`
- [x] Pass `payload_fn` through to `WriteIndex()` (update private method signature)
- [x] In `WriteIndex()` loop: call `payload_fn(idx)` instead of passing `{}`
- [x] Guard: if `payload_fn == nullptr`, pass empty vector (backward compat)

**Files**: `include/vdb/index/ivf_builder.h`, `src/index/ivf_builder.cpp`

## Task 3: IvfBuilder -- serialize payload_schemas to segment.meta
- [x] In `WriteIndex()` segment.meta section: serialize `config_.payload_schemas` as `[PayloadColumnSchema]`
- [x] Pass the vector to `CreateSegmentMeta()`

**Files**: `src/index/ivf_builder.cpp`

## Task 4: IvfIndex::Open -- deserialize payload_schemas
- [x] In `Open()`: read `payload_schemas` from FlatBuffers `seg_meta`
- [x] Populate `payload_schemas_` member from deserialized data
- [x] `segment_.Open(dir, payload_schemas_)` now receives actual schemas
- [x] Handle missing field gracefully (old segment.meta → empty schemas)

**Files**: `src/index/ivf_index.cpp`

## Task 5: Payload pipeline unit tests
- [x] Test: Build with PayloadFn (2 columns: INT64 id + BYTES data) → Open → OverlapScheduler::Search → verify payload[0] is correct INT64 id, payload[1] is correct BYTES content
- [x] Test: Build without PayloadFn → Open → Search → payload is empty
- [x] Test: Verify segment.meta round-trip: build with schemas → open → check schemas match
- [x] Test: Build with PayloadFn (INT64 id + STRING text) → verify STRING modality works too

**Files**: `tests/index/ivf_builder_test.cpp` (extend), `tests/query/overlap_scheduler_test.cpp` (extend)

## Task 6: NpyReader implementation
- [x] Implement `include/vdb/io/npy_reader.h` (NpyArrayFloat, NpyArrayInt64, LoadNpyFloat32, LoadNpyInt64)
- [x] Implement `src/io/npy_reader.cpp` (npy magic, header parsing, raw data loading)
- [x] Support npy v1.0 and v2.0 header formats
- [x] Error on unsupported dtype, big-endian, Fortran order

**Files**: `include/vdb/io/npy_reader.h`, `src/io/npy_reader.cpp`

## Task 7: JsonlReader implementation
- [x] Implement `include/vdb/io/jsonl_reader.h` (ReadJsonlLines)
- [x] Implement `src/io/jsonl_reader.cpp` (line-by-line ifstream reader)

**Files**: `include/vdb/io/jsonl_reader.h`, `src/io/jsonl_reader.cpp`

## Task 8: Build system -- vdb_io library + tests
- [x] Add `vdb_io` static library target to CMakeLists.txt
- [x] Add `test_npy_reader` and `test_jsonl_reader` test targets
- [x] Link `vdb_io` against `vdb_common`

**Files**: `CMakeLists.txt`

## Task 9: Npy/Jsonl unit tests
- [x] Test: LoadNpyFloat32 with coco_1k/image_embeddings.npy → shape (1000, 512), dtype float32
- [x] Test: LoadNpyInt64 with coco_1k/image_ids.npy → count 1000
- [x] Test: LoadNpyFloat32 with invalid file → error status
- [x] Test: ReadJsonlLines with coco_1k/metadata.jsonl → 1000 lines, first line contains "image_id": 139

**Files**: `tests/io/npy_reader_test.cpp`, `tests/io/jsonl_reader_test.cpp`

## Dependency order

```
Task 1 (FlatBuffers schema)
  └── Task 2 (PayloadFn) + Task 3 (serialize)
        └── Task 4 (deserialize)
              └── Task 5 (payload tests)

Task 6 (NpyReader) ──┐
Task 7 (JsonlReader) ─┼── Task 8 (CMake) ── Task 9 (io tests)
                      │
                      └── (independent of Tasks 1-5)
```
