# Design: Payload Pipeline + Npy Loader

## Part A: Payload Pipeline Fix

### A1. IvfBuilder::Build() -- PayloadFn callback

Add a callback type and an overloaded `Build()`:

```cpp
// include/vdb/index/ivf_builder.h

/// Callback to supply payload for a given vector index.
/// Returns empty vector if no payload for this vector.
using PayloadFn = std::function<std::vector<Datum>(uint32_t vec_index)>;

Status Build(const float* vectors, uint32_t N, Dim dim,
             const std::string& output_dir,
             PayloadFn payload_fn = nullptr);
```

**Why callback instead of array**: For large datasets, pre-building an `N x num_cols` payload array is wasteful. The callback lets the caller lazily produce payloads (e.g., reading from jsonl on demand).

**Backward compatibility**: `payload_fn = nullptr` means no payload -- existing callers don't change.

### A2. WriteIndex() -- pass payload through

In `ivf_builder.cpp`, `WriteIndex()` receives the `PayloadFn` and uses it:

```
Current:  dat_writer.WriteRecord(vec, {}, entry);
Fixed:    auto pl = payload_fn ? payload_fn(idx) : std::vector<Datum>{};
          dat_writer.WriteRecord(vec, pl, entry);
```

`WriteIndex()` signature gains the `PayloadFn` parameter (private method, no API impact).

### A3. FlatBuffers schema -- PayloadColumnSchema

Add to `schema/segment_meta.fbs`:

```fbs
table PayloadColumnSchema {
    id: uint32;
    name: string;
    dtype: uint8;      // Maps to C++ DType enum value
    nullable: bool;
}
```

Add field to `SegmentMeta`:

```fbs
table SegmentMeta {
    ...existing fields...
    payload_schemas: [PayloadColumnSchema];  // NEW
}
```

FlatBuffers guarantees backward compatibility for added fields (old readers skip unknown fields, new readers get null for missing fields).

### A4. Build-side serialization (ivf_builder.cpp)

In the segment.meta writing section of `WriteIndex()`, serialize `config_.payload_schemas`:

```
for each ColumnSchema in config_.payload_schemas:
    CreatePayloadColumnSchema(fbb, id, name, dtype, nullable)
CreateVector(schema_offsets) → pass to CreateSegmentMeta
```

### A5. Open-side deserialization (ivf_index.cpp)

In `IvfIndex::Open()`, after reading seg_meta:

```
if seg_meta->payload_schemas() exists:
    for each entry:
        push_back to payload_schemas_
segment_.Open(dir, payload_schemas_)   // DataFileReader now knows the layout
```

### A6. Payload schema convention: two fixed columns

All datasets use a standard two-column payload layout:

```
col 0: { id: 0, name: "id",   dtype: INT64 }   — record identity, used for recall verification
col 1: { id: 1, name: "data", dtype: BYTES }    — raw original data (image/audio/video/text bytes)
                                 or STRING          (for plain-text modalities)
```

This convention is **not enforced by the engine** — `payload_schemas` is fully flexible.
But all test harnesses and the coco_1k e2e test follow this layout.

Record layout by modality:

```
Image (coco_1k):
  [float[512] embedding] [int64 image_id] [uint32 len] [raw jpg bytes ~158KB]

Audio:
  [float[dim] embedding] [int64 audio_id] [uint32 len] [raw wav/mp3 bytes]

Text (dtype=STRING for col 1):
  [float[dim] embedding] [int64 doc_id]   [uint32 len] [text chars]
```

Query-side I/O behavior with large payloads:


### Data flow after fix

```
BUILD:
  PayloadFn(idx) ──► WriteRecord(vec, [id, data]) ──► data.dat
  config_.payload_schemas ──► segment.meta (FlatBuffers)

OPEN:
  segment.meta ──► payload_schemas_ ──► Segment::Open(dir, schemas)
                                         └── DataFileReader.Open(path, dim, schemas)

QUERY (unchanged):
  ProbeCluster: addr.size > vec_bytes → VEC_ALL / PAYLOAD reads
  AssembleResults: ParsePayload(buf, schemas) → SearchResult.payload
                   payload[0].fixed.i64 = id
                   payload[1].var_data   = raw bytes
```

## Part B: Npy/Jsonl Loader

### B1. NpyReader

Location: `include/vdb/io/npy_reader.h`, `src/io/npy_reader.cpp`

Supports only the two dtypes needed:
- `<f4` (float32) -- for embeddings
- `<i8` (int64) -- for IDs

Design:
```cpp
namespace vdb::io {

struct NpyArrayFloat {
    std::vector<float> data;   // row-major
    uint32_t rows;
    uint32_t cols;             // 1 for 1-D arrays
};

struct NpyArrayInt64 {
    std::vector<int64_t> data;
    uint32_t count;
};

/// Load a float32 .npy file. Returns error if dtype != <f4.
StatusOr<NpyArrayFloat> LoadNpyFloat32(const std::string& path);

/// Load an int64 .npy file. Returns error if dtype != <i8.
StatusOr<NpyArrayInt64> LoadNpyInt64(const std::string& path);

}  // namespace vdb::io
```

Npy format parsing:
1. Read 6-byte magic `\x93NUMPY`
2. Read 2-byte version (1.0 or 2.0)
3. Read header length (2 bytes for v1, 4 bytes for v2)
4. Parse Python dict string for `descr`, `fortran_order`, `shape`
5. Read raw data

### B2. JsonlReader

Location: `include/vdb/io/jsonl_reader.h`, `src/io/jsonl_reader.cpp`

Minimal line-by-line reader. Does NOT include a JSON parser -- returns raw line strings. The caller parses fields as needed (e.g., with a lightweight JSON lib or manual extraction for simple cases).

```cpp
namespace vdb::io {

/// Iterate lines of a .jsonl file, calling fn(line_number, line_content).
Status ReadJsonlLines(const std::string& path,
                      std::function<void(uint32_t, std::string_view)> fn);

}  // namespace vdb::io
```

For the coco_1k use case, the test harness will extract `image_id`, `caption`, etc. from lines using simple string search (the JSON structure is flat and predictable).

### B3. Build system

Add to `CMakeLists.txt`:
- `vdb_io` library: `src/io/npy_reader.cpp`, `src/io/jsonl_reader.cpp`
- Test targets: `test_npy_reader`, `test_jsonl_reader`

## Tradeoffs

| Decision | Alternative | Why chosen |
|----------|------------|------------|
| PayloadFn callback | Payload array parameter | Memory efficient for large datasets; caller controls lifecycle |
| FlatBuffers schema for payload_schemas | Open() parameter | Self-describing index; Open() doesn't need external knowledge |
| Minimal JsonlReader (returns strings) | Full JSON parser | Avoids adding nlohmann/json dependency; flat JSON is trivial to extract |
| NpyReader only <f4 and <i8 | Full numpy support | Only these two dtypes appear in real datasets; YAGNI |
