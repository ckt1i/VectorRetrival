# Proposal: Payload Pipeline + Npy Loader

## Problem

The IVF index build pipeline currently writes empty payloads to `data.dat`. Three breakpoints prevent payload data from flowing through the system:

1. **`IvfBuilder::Build()`** has no way to accept per-vector payload data -- `WriteRecord(vec, {}, entry)` always passes empty payload.
2. **`segment.meta`** (FlatBuffers) does not serialize `payload_schemas`, so the schema information is lost after build.
3. **`IvfIndex::Open()`** never restores `payload_schemas_`, so `DataFileReader` opens with empty schemas and cannot parse payloads.

The query-side code (`OverlapScheduler`, `RerankConsumer`, `AssembleResults`) already fully supports payload reading -- the data just never gets written or described.

Additionally, there is no utility to load `.npy` / `.jsonl` files, which are needed for end-to-end testing with the coco_1k dataset.

## Solution

### Part A: Fix Payload Pipeline (3 breakpoints)

1. Add a `PayloadFn` callback parameter to `IvfBuilder::Build()` so callers can supply per-vector payloads without pre-allocating the entire array.
2. In `WriteIndex()`, call `PayloadFn(vec_index)` for each record instead of passing `{}`.
3. Add `PayloadColumnSchema` table to `segment_meta.fbs` and serialize `payload_schemas` during build, deserialize during `Open()`.

### Part B: Npy/Jsonl Loader (project-level utility)

Add `include/vdb/io/npy_reader.h` and `include/vdb/io/jsonl_reader.h` as lightweight, project-level I/O utilities for loading test and real-world datasets.

## Non-goals

- Changing the `data.dat` on-disk record format (it already supports payloads).
- Modifying `DataFileWriter`, `DataFileReader`, `ClusterStoreWriter`, `ClusterStoreReader` (they already work).
- Modifying the query pipeline (`OverlapScheduler`, `RerankConsumer`).
- Adding `vector_id` as a first-class field in `SearchResult` (deferred; use payload column for now).

## Scope

- 5 files modified, 4 new files, 1 FlatBuffers schema change
- Zero changes to storage layer or query pipeline
- Backward compatible: existing Build() calls with no payload continue to work (PayloadFn defaults to nullptr)
