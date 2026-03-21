#include "vdb/io/jsonl_reader.h"

#include <fstream>
#include <string>

namespace vdb {
namespace io {

Status ReadJsonlLines(const std::string& path,
                      std::function<void(uint32_t, std::string_view)> fn) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return Status::IOError("Failed to open jsonl file: " + path);
    }

    std::string line;
    uint32_t line_num = 0;
    while (std::getline(f, line)) {
        // Skip empty lines
        if (line.empty()) continue;
        // Skip lines that are only whitespace
        bool all_ws = true;
        for (char c : line) {
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                all_ws = false;
                break;
            }
        }
        if (all_ws) continue;

        fn(line_num, line);
        ++line_num;
    }

    return Status::OK();
}

}  // namespace io
}  // namespace vdb
