#pragma once
#include "core/package.h"

namespace extract {
struct ExtractionResult {
    fs::path output;
    fs::path primary_output;
    bool complete = true;
    std::wstring details;
    std::uint64_t total_bytes = 0;
    std::size_t file_count = 0;
};
// 获得 Package 所有权，写完本层就释放输入/缓存；所有内层共享一个总预算。
ExtractionResult extract_tree(std::unique_ptr<Package> package, const fs::path& output_parent = {});
} // namespace extract
