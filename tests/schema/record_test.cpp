// =============================================================================
// Record Schema Tests
// =============================================================================
// 验证 record.fbs 的序列化/反序列化
// =============================================================================

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <string>

// FlatBuffers generated headers
#include "record_generated.h"

namespace vdb {
namespace test {

namespace schema = vdb::schema;

// =============================================================================
// record.fbs Tests
// =============================================================================

class RecordSchemaTest : public ::testing::Test {
protected:
    flatbuffers::FlatBufferBuilder builder_{1024};
};

// ---------------------------------------------------------------------------
// PayloadMode enum
// ---------------------------------------------------------------------------

TEST_F(RecordSchemaTest, PayloadModeEnumValues) {
    EXPECT_EQ(static_cast<int>(schema::PayloadMode::INLINE), 0);
    EXPECT_EQ(static_cast<int>(schema::PayloadMode::EXTERN), 1);
}

// ---------------------------------------------------------------------------
// ExternRef struct
// ---------------------------------------------------------------------------

TEST_F(RecordSchemaTest, ExternRefStruct) {
    schema::ExternRef ref(42, 1024000, 8192);
    EXPECT_EQ(ref.file_id(), 42);
    EXPECT_EQ(ref.offset(), 1024000);
    EXPECT_EQ(ref.length(), 8192);
}

// ---------------------------------------------------------------------------
// Record with inline payload
// ---------------------------------------------------------------------------

TEST_F(RecordSchemaTest, CreateRecordWithInlinePayload) {
    // Simulate a small payload (< 4KB)
    std::vector<uint8_t> payload_bytes(256, 0xAB);
    auto payload_data = builder_.CreateVector(payload_bytes);
    auto inline_payload = schema::CreateInlinePayload(builder_, payload_data);

    // Simulate vector (PQ codes, 16 bytes for M=16, nbits=8)
    std::vector<uint8_t> vec_data(16, 0x42);
    auto vector = builder_.CreateVector(vec_data);

    auto record = schema::CreateRecord(
        builder_,
        vector,                                // vector
        schema::PayloadMode::INLINE,           // mode
        schema::Payload::InlinePayload,        // payload_type
        inline_payload.Union(),                // payload
        1001,                                  // vec_id
        1700000000000000ULL,                   // timestamp
        256                                    // payload_size
    );

    builder_.Finish(record, "VREC");

    // Verify file identifier
    EXPECT_TRUE(flatbuffers::BufferHasIdentifier(
        builder_.GetBufferPointer(), "VREC"));

    // Read back
    auto* read = schema::GetRecord(builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->vec_id(), 1001);
    EXPECT_EQ(read->mode(), schema::PayloadMode::INLINE);
    EXPECT_EQ(read->payload_type(), schema::Payload::InlinePayload);
    EXPECT_EQ(read->payload_size(), 256);

    // Check vector data
    ASSERT_NE(read->vector(), nullptr);
    EXPECT_EQ(read->vector()->size(), 16);
    EXPECT_EQ(read->vector()->Get(0), 0x42);

    // Check inline payload data
    auto* ip = read->payload_as_InlinePayload();
    ASSERT_NE(ip, nullptr);
    ASSERT_NE(ip->data(), nullptr);
    EXPECT_EQ(ip->data()->size(), 256);
    EXPECT_EQ(ip->data()->Get(0), 0xAB);
}

// ---------------------------------------------------------------------------
// Record with extern payload
// ---------------------------------------------------------------------------

TEST_F(RecordSchemaTest, CreateRecordWithExternPayload) {
    // Extern ref: file_id=3, offset=65536, length=1048576
    schema::ExternRef ext_ref(3, 65536, 1048576);
    auto extern_payload = schema::CreateExternPayload(builder_, &ext_ref);

    // Vector data
    std::vector<uint8_t> vec_data(16, 0x99);
    auto vector = builder_.CreateVector(vec_data);

    auto record = schema::CreateRecord(
        builder_,
        vector,
        schema::PayloadMode::EXTERN,
        schema::Payload::ExternPayload,
        extern_payload.Union(),
        2002,
        1700000001000000ULL,
        1048576
    );

    builder_.Finish(record, "VREC");

    auto* read = schema::GetRecord(builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->vec_id(), 2002);
    EXPECT_EQ(read->mode(), schema::PayloadMode::EXTERN);
    EXPECT_EQ(read->payload_type(), schema::Payload::ExternPayload);
    EXPECT_EQ(read->payload_size(), 1048576);

    // Check extern payload
    auto* ep = read->payload_as_ExternPayload();
    ASSERT_NE(ep, nullptr);
    ASSERT_NE(ep->ref(), nullptr);
    EXPECT_EQ(ep->ref()->file_id(), 3);
    EXPECT_EQ(ep->ref()->offset(), 65536);
    EXPECT_EQ(ep->ref()->length(), 1048576);
}

// ---------------------------------------------------------------------------
// ExternBlobEntry
// ---------------------------------------------------------------------------

TEST_F(RecordSchemaTest, CreateExternBlobEntry) {
    auto entry = schema::CreateExternBlobEntry(
        builder_,
        5,              // file_id
        0,              // offset
        4096,           // length
        0xDEADBEEF      // checksum
    );

    builder_.Finish(entry);

    auto* read = flatbuffers::GetRoot<schema::ExternBlobEntry>(
        builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->file_id(), 5);
    EXPECT_EQ(read->offset(), 0);
    EXPECT_EQ(read->length(), 4096);
    EXPECT_EQ(read->checksum(), 0xDEADBEEF);
}

// ---------------------------------------------------------------------------
// FileNameDirectory
// ---------------------------------------------------------------------------

TEST_F(RecordSchemaTest, CreateFileNameDirectory) {
    auto name0 = builder_.CreateString("segment_1/blob_0.dat");
    auto name1 = builder_.CreateString("segment_1/blob_1.dat");

    std::vector<flatbuffers::Offset<schema::FileNameEntry>> entries;
    entries.push_back(schema::CreateFileNameEntry(
        builder_, 0, name0, 134217728, 0x11111111));
    entries.push_back(schema::CreateFileNameEntry(
        builder_, 1, name1, 268435456, 0x22222222));

    auto entries_vec = builder_.CreateVector(entries);
    auto dir = schema::CreateFileNameDirectory(builder_, entries_vec);

    builder_.Finish(dir);

    auto* read = flatbuffers::GetRoot<schema::FileNameDirectory>(
        builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    ASSERT_NE(read->entries(), nullptr);
    EXPECT_EQ(read->entries()->size(), 2);

    auto* e0 = read->entries()->Get(0);
    EXPECT_EQ(e0->file_id(), 0);
    EXPECT_STREQ(e0->file_name()->c_str(), "segment_1/blob_0.dat");
    EXPECT_EQ(e0->file_size(), 134217728);

    auto* e1 = read->entries()->Get(1);
    EXPECT_EQ(e1->file_id(), 1);
    EXPECT_STREQ(e1->file_name()->c_str(), "segment_1/blob_1.dat");
}

// ---------------------------------------------------------------------------
// RecordFileHeader
// ---------------------------------------------------------------------------

TEST_F(RecordSchemaTest, CreateRecordFileHeader) {
    // Create empty blob directory
    std::vector<flatbuffers::Offset<schema::FileNameEntry>> dir_entries;
    auto dir_vec = builder_.CreateVector(dir_entries);
    auto blob_dir = schema::CreateFileNameDirectory(builder_, dir_vec);

    auto header = schema::CreateRecordFileHeader(
        builder_,
        0x56524543,     // magic ("VREC")
        1,              // version
        100000,         // num_records
        8192,           // offset_index_offset
        800000,         // offset_index_length
        blob_dir,       // blob_directory
        40960000,       // total_inline_bytes
        204800000,      // total_extern_bytes
        90000,          // inline_count
        10000,          // extern_count
        1700000000ULL,  // create_time
        0               // checksum
    );

    builder_.Finish(header);

    auto* read = flatbuffers::GetRoot<schema::RecordFileHeader>(
        builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->magic(), 0x56524543);
    EXPECT_EQ(read->version(), 1);
    EXPECT_EQ(read->num_records(), 100000);
    EXPECT_EQ(read->inline_count(), 90000);
    EXPECT_EQ(read->extern_count(), 10000);
    EXPECT_EQ(read->total_inline_bytes(), 40960000);
    EXPECT_EQ(read->total_extern_bytes(), 204800000);
}

// ---------------------------------------------------------------------------
// Integration: inline vs extern decision boundary
// ---------------------------------------------------------------------------

TEST(RecordIntegrationTest, InlineExternBoundary) {
    flatbuffers::FlatBufferBuilder builder(4096);
    constexpr size_t kThreshold = 4096;

    // --- Below threshold: inline ---
    {
        std::vector<uint8_t> small_payload(kThreshold - 1, 0xCC);
        auto payload_data = builder.CreateVector(small_payload);
        auto inline_p = schema::CreateInlinePayload(builder, payload_data);

        std::vector<uint8_t> vec(16, 0x01);
        auto vector = builder.CreateVector(vec);

        auto rec = schema::CreateRecord(
            builder, vector,
            schema::PayloadMode::INLINE,
            schema::Payload::InlinePayload,
            inline_p.Union(),
            1, 0, static_cast<uint32_t>(kThreshold - 1));

        builder.Finish(rec, "VREC");

        auto* r = schema::GetRecord(builder.GetBufferPointer());
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->mode(), schema::PayloadMode::INLINE);
        EXPECT_EQ(r->payload_size(), kThreshold - 1);

        auto* ip = r->payload_as_InlinePayload();
        ASSERT_NE(ip, nullptr);
        EXPECT_EQ(ip->data()->size(), kThreshold - 1);
    }

    builder.Clear();

    // --- At threshold: extern ---
    {
        schema::ExternRef ext_ref(0, 0, static_cast<uint32_t>(kThreshold));
        auto extern_p = schema::CreateExternPayload(builder, &ext_ref);

        std::vector<uint8_t> vec(16, 0x02);
        auto vector = builder.CreateVector(vec);

        auto rec = schema::CreateRecord(
            builder, vector,
            schema::PayloadMode::EXTERN,
            schema::Payload::ExternPayload,
            extern_p.Union(),
            2, 0, static_cast<uint32_t>(kThreshold));

        builder.Finish(rec, "VREC");

        auto* r = schema::GetRecord(builder.GetBufferPointer());
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->mode(), schema::PayloadMode::EXTERN);
        EXPECT_EQ(r->payload_size(), kThreshold);

        auto* ep = r->payload_as_ExternPayload();
        ASSERT_NE(ep, nullptr);
        EXPECT_EQ(ep->ref()->length(), kThreshold);
    }
}

// =============================================================================
// Phase 2.5: DataFileHeader and ColumnSchemaEntry Tests
// =============================================================================

TEST_F(RecordSchemaTest, CreateColumnSchemaEntry) {
    auto name = builder_.CreateString("user_name");
    auto entry = schema::CreateColumnSchemaEntry(
        builder_,
        0,          // column_id
        name,       // name
        11          // data_type (STRING = 11 in columns.fbs DataType)
    );
    
    builder_.Finish(entry);
    
    auto* read = flatbuffers::GetRoot<schema::ColumnSchemaEntry>(
        builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->column_id(), 0);
    EXPECT_STREQ(read->name()->c_str(), "user_name");
    EXPECT_EQ(read->data_type(), 11);
}

TEST_F(RecordSchemaTest, CreateDataFileHeader) {
    // Create column schemas
    auto name0 = builder_.CreateString("user_id");
    auto name1 = builder_.CreateString("user_name");
    auto name2 = builder_.CreateString("embedding");
    
    std::vector<flatbuffers::Offset<schema::ColumnSchemaEntry>> col_entries;
    col_entries.push_back(schema::CreateColumnSchemaEntry(builder_, 0, name0, 4));  // INT64
    col_entries.push_back(schema::CreateColumnSchemaEntry(builder_, 1, name1, 11)); // STRING
    col_entries.push_back(schema::CreateColumnSchemaEntry(builder_, 2, name2, 9));  // FLOAT32
    auto col_schemas = builder_.CreateVector(col_entries);
    
    // Create column region offsets
    std::vector<uint64_t> offsets = {4096, 8192, 16384};
    auto region_offsets = builder_.CreateVector(offsets);
    
    auto header = schema::CreateDataFileHeader(
        builder_,
        0x56444154,     // magic ("VDAT")
        1,              // version
        42,             // cluster_id
        10000,          // num_records
        128,            // dimension
        col_schemas,    // column_schemas
        region_offsets, // column_region_offsets
        0               // checksum
    );
    
    builder_.Finish(header);
    
    auto* read = flatbuffers::GetRoot<schema::DataFileHeader>(
        builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->magic(), 0x56444154);
    EXPECT_EQ(read->version(), 1);
    EXPECT_EQ(read->cluster_id(), 42);
    EXPECT_EQ(read->num_records(), 10000);
    EXPECT_EQ(read->dimension(), 128);
    
    // Check column schemas
    ASSERT_NE(read->column_schemas(), nullptr);
    EXPECT_EQ(read->column_schemas()->size(), 3);
    
    auto* cs0 = read->column_schemas()->Get(0);
    EXPECT_EQ(cs0->column_id(), 0);
    EXPECT_STREQ(cs0->name()->c_str(), "user_id");
    EXPECT_EQ(cs0->data_type(), 4);
    
    auto* cs1 = read->column_schemas()->Get(1);
    EXPECT_STREQ(cs1->name()->c_str(), "user_name");
    EXPECT_EQ(cs1->data_type(), 11);
    
    // Check region offsets
    ASSERT_NE(read->column_region_offsets(), nullptr);
    EXPECT_EQ(read->column_region_offsets()->size(), 3);
    EXPECT_EQ(read->column_region_offsets()->Get(0), 4096);
    EXPECT_EQ(read->column_region_offsets()->Get(1), 8192);
    EXPECT_EQ(read->column_region_offsets()->Get(2), 16384);
}

TEST_F(RecordSchemaTest, DataFileHeaderEmptyColumns) {
    // DataFile with no payload columns (vector-only)
    std::vector<flatbuffers::Offset<schema::ColumnSchemaEntry>> empty_cols;
    auto col_schemas = builder_.CreateVector(empty_cols);
    std::vector<uint64_t> empty_offsets;
    auto region_offsets = builder_.CreateVector(empty_offsets);
    
    auto header = schema::CreateDataFileHeader(
        builder_,
        0x56444154, 1, 0,
        5000,       // num_records
        256,        // dimension (high-dim)
        col_schemas,
        region_offsets,
        0
    );
    
    builder_.Finish(header);
    
    auto* read = flatbuffers::GetRoot<schema::DataFileHeader>(
        builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->num_records(), 5000);
    EXPECT_EQ(read->dimension(), 256);
    EXPECT_EQ(read->column_schemas()->size(), 0);
    EXPECT_EQ(read->column_region_offsets()->size(), 0);
}

}  // namespace test
}  // namespace vdb
