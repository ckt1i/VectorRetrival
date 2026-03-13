#include "vdb/storage/segment.h"

namespace vdb {
namespace storage {

// ============================================================================
// Construction / Destruction
// ============================================================================

Segment::Segment() = default;

Segment::~Segment() = default;

// ============================================================================
// Open — open the unified cluster.clu + data.dat
// ============================================================================

Status Segment::Open(const std::string& dir,
                     const std::vector<ColumnSchema>& payload_schemas) {
    const std::string clu_path = dir + "/cluster.clu";
    const std::string dat_path = dir + "/data.dat";

    // Open cluster store (reads header + lookup table)
    VDB_RETURN_IF_ERROR(clu_reader_.Open(clu_path));

    // Open data file
    VDB_RETURN_IF_ERROR(dat_reader_.Open(dat_path,
                                          clu_reader_.dim(),
                                          payload_schemas));

    return Status::OK();
}

}  // namespace storage
}  // namespace vdb
