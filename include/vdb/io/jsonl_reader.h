#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "vdb/common/status.h"

namespace vdb {
namespace io {

/// Iterate lines of a .jsonl file, calling fn(line_number, line_content).
/// Skips empty lines. Returns IOError if file cannot be opened.
Status ReadJsonlLines(const std::string& path,
                      std::function<void(uint32_t, std::string_view)> fn);

}  // namespace io
}  // namespace vdb
