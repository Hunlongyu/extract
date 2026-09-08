#pragma once
#include "io/output.h"
#include "io/input.h"
#include <functional>

namespace extract::codecs {
struct CabMember { std::string name; std::wstring decoded_name; std::uint64_t size; std::uint32_t offset = 0; std::uint16_t folder = 0; };
std::wstring cabinet_path(const std::string& name);
std::string cabinet_name(const std::wstring& name);
class CabinetSet {
public:
    // 所有视图由调用方持有读锁并保证生命周期；FDI 不能自行访问文件系统。
    CabinetSet(io::Bytes first, std::string name, const std::function<io::Bytes(const std::string&)>& resolver);
    ~CabinetSet();
    const std::vector<CabMember>& members() const;
    std::size_t volume_count() const;
    void extract(std::span<Entry*> targets, io::Output& output);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::vector<CabMember> cab_members(io::Bytes bytes);
void extract_cab(io::Bytes bytes, std::span<Entry*> targets, io::Output& output);
std::vector<std::byte> cab_member_bytes(io::Bytes bytes, std::size_t index, std::size_t limit);
void verify_file(Entry& entry, const fs::path& path);
}
