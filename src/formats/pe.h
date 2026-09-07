#pragma once
#include "io/input.h"

namespace extract::formats {
struct PeSection { std::string name; std::uint32_t offset, size; };
struct PeLayout { std::vector<PeSection> sections; std::uint64_t overlay = 0; std::uint32_t certificate_offset = 0, certificate_size = 0; };
PeLayout pe_layout(io::Bytes input);
// 只解析 PE 数据，不将输入作为模块加载；返回指定数值 ID 的 RCDATA 内容。
io::Bytes pe_rcdata(io::Bytes input, std::uint32_t id);
} // namespace extract::formats
