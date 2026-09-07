#include "formats/cab.h"
#include "formats/pe.h"
#include "codecs/cab.h"
#include <algorithm>
namespace extract::formats {
std::optional<CabLocation> cab_probe(io::Bytes input) {
    const auto signature = [&](std::size_t offset) {
        return input.size() - offset >= 4 && io::Reader(input.subspan(offset)).u32() == 0x4643534d;
    };
    if (signature(0)) return CabLocation{0, input.size()};
    if (input.size() < 64 || input[0] != std::byte{'M'} || input[1] != std::byte{'Z'}) return {};
    const auto pe = pe_layout(input);
    const auto end = (std::min)(input.size(), static_cast<std::size_t>(pe.overlay) + 65536);
    for (auto offset = static_cast<std::size_t>(pe.overlay); offset < end; ++offset) {
        if (!signature(offset) || input.size() - offset < 36) continue;
        io::Reader header(input.subspan(offset)); header.skip(4);
        if (header.u32() != 0) continue;
        const auto size = header.u32();
        require(size >= 36 && size <= input.size() - offset, Status::corrupt, L"CAB SFX 载荷长度无效。");
        require(!pe.certificate_offset || offset + size <= pe.certificate_offset || offset >=
                static_cast<std::uint64_t>(pe.certificate_offset) + pe.certificate_size, Status::corrupt, L"CAB 载荷与 PE 证书重叠。");
        return CabLocation{offset, size};
    }
    return {};
}
struct CabPackage::Impl {
    io::Input input;
    io::Bytes bytes;
    Catalog catalog;
    explicit Impl(const fs::path& path) : input(path) {
        const auto location = cab_probe(input.bytes());
        require(location.has_value(), Status::unsupported, L"没有找到标准 CAB 载荷。");
        bytes = io::slice(input.bytes(), location->offset, location->size);
        catalog.input = input.path(); catalog.format = L"CAB"; catalog.format_version = L"1.3";
        const auto members = codecs::cab_members(bytes);
        for (const auto& member : members) {
            Entry entry; entry.id = L"cab-" + std::to_wstring(catalog.files.size());
            entry.path = entry.original_path = member.decoded_name; platform::validate_relative(entry.path);
            entry.size = member.size; entry.source_expression = member.decoded_name;
            catalog.total_size += entry.size; catalog.files.push_back(std::move(entry));
        }
        if (location->offset) catalog.notes.push_back(L"静态提取 PE 附加区 CAB；不执行 SFX 指令。");
        plan_paths(catalog);
    }
};
CabPackage::CabPackage(const fs::path& path) : impl_(std::make_unique<Impl>(path)) {}
CabPackage::~CabPackage() = default;
const Catalog& CabPackage::catalog() const { return impl_->catalog; }
fs::path CabPackage::extract(const fs::path& parent) {
    auto& catalog = impl_->catalog;
    io::Output output(parent.empty() ? catalog.input.parent_path() : parent, catalog.input.stem().wstring());
    std::vector<Entry*> targets; for (auto& entry : catalog.files) targets.push_back(&entry);
    codecs::extract_cab(impl_->bytes, targets, output);
    const auto report = platform::utf8(catalog_json(catalog, true));
    { auto file = output.create_file(L"_extract-report.json"); platform::write_all(file.get(), std::as_bytes(std::span(report)));
      if (!FlushFileBuffers(file.get())) platform::io_failure(L"保存 CAB 清单失败"); }
    return output.commit();
}
}
