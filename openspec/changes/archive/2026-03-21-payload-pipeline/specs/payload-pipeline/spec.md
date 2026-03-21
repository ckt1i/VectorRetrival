# Spec: Payload Pipeline + Npy Loader

## Requirements

### R1: IvfBuilder accepts per-vector payloads via callback

- `Build()` accepts an optional `PayloadFn` parameter (defaults to `nullptr`).
- When `PayloadFn` is provided, each record in `data.dat` includes payload columns.
- When `PayloadFn` is `nullptr`, behavior is identical to current (empty payload).
- `PayloadFn` signature: `std::function<std::vector<Datum>(uint32_t vec_index)>`
- `config_.payload_schemas` must be set to match the columns returned by `PayloadFn`.

### R2: Payload schemas persisted in segment.meta

- `segment_meta.fbs` includes a `PayloadColumnSchema` table with fields: `id`, `name`, `dtype`, `nullable`.
- `SegmentMeta` includes a `payload_schemas` vector field.
- `IvfBuilder` serializes `config_.payload_schemas` into segment.meta during build.
- Old segment.meta files without `payload_schemas` remain valid (FlatBuffers compatibility).

### R3: IvfIndex::Open() restores payload schemas

- `Open()` reads `payload_schemas` from segment.meta into `payload_schemas_` member.
- `payload_schemas_` is passed to `Segment::Open()` and subsequently to `DataFileReader::Open()`.
- If segment.meta has no `payload_schemas` field, `payload_schemas_` remains empty (backward compatible).

### R4: NpyReader loads float32 and int64 arrays

- `LoadNpyFloat32(path)` returns `StatusOr<NpyArrayFloat>` with row-major data, rows, cols.
- `LoadNpyInt64(path)` returns `StatusOr<NpyArrayInt64>` with data and count.
- Supports npy format version 1.0 and 2.0.
- Returns error for unsupported dtypes, big-endian, or Fortran order.

### R5: JsonlReader iterates lines

- `ReadJsonlLines(path, callback)` reads file line by line, invoking callback with `(line_number, line_content)`.
- Skips empty lines.
- Returns `Status::IOError` if file cannot be opened.

### R6: Backward compatibility

- All existing tests pass without modification (Build() without PayloadFn compiles and works).
- Old segment.meta files (without payload_schemas) load correctly.
- No changes to DataFileWriter, DataFileReader, ClusterStore*, OverlapScheduler, or RerankConsumer.

### R7: Standard two-column payload convention

- All test harnesses use a standard two-column payload layout:
  - Column 0: `{id: 0, name: "id", dtype: INT64}` — record identity for recall verification.
  - Column 1: `{id: 1, name: "data", dtype: BYTES or STRING}` — raw original data.
- Column 1 dtype varies by modality: BYTES for binary data (image/audio/video), STRING for text.
- This convention is not enforced by the engine — payload_schemas is fully flexible.

## Acceptance Criteria

1. Unit test: Build with PayloadFn (INT64 id + BYTES image) → Open → ReadRecord → payload[0] is correct id, payload[1] is correct image bytes.
2. Unit test: Build without PayloadFn → Open → results have empty payload (backward compat).
3. Unit test: LoadNpyFloat32 reads coco_1k/image_embeddings.npy → shape (1000, 512).
4. Unit test: LoadNpyInt64 reads coco_1k/image_ids.npy → count 1000.
5. Unit test: ReadJsonlLines reads coco_1k/metadata.jsonl → 1000 lines.
6. Integration: Build coco_1k (1000 vectors, dim=512, nlist=32, payload=[id INT64, image BYTES]) → Search → result payload[0] matches ground truth image_id, payload[1] is valid jpg bytes.
