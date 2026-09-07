#pragma once
#include "io/output.h"
#include "io/input.h"

namespace extract::codecs {
struct CabMember { std::string name; std::wstring decoded_name; std::uint64_t size; };
std::vector<CabMember> cab_members(io::Bytes bytes);
void extract_cab(io::Bytes bytes, std::span<Entry*> targets, io::Output& output);
std::vector<std::byte> cab_member_bytes(io::Bytes bytes, std::size_t index, std::size_t limit);
void verify_file(Entry& entry, const fs::path& path);
}
