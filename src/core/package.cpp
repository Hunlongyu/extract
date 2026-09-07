#include "core/package.h"
#include "formats/msi.h"
#include "formats/inno.h"
#include "formats/nsis.h"
#include "formats/archive.h"
#include "formats/cab.h"
#include "formats/burn.h"
#include "io/input.h"
#include "platform/log.h"
#include <map>

namespace extract {
namespace {
template<class T>
std::unique_ptr<Package> parse(const fs::path& path, std::wstring_view format) {
    log::Scope step(L"package.parse", &path);
    log::write(log::Level::info, L"format.candidate", format);
    try {
        auto package = std::make_unique<T>(path);
        const auto& catalog = package->catalog();
        log::detail(log::Level::info, L"catalog.ready", [&] {
            return L"format=" + catalog.format + L"; version=" + catalog.format_version + L"; files=" +
                std::to_wstring(catalog.files.size()) + L"; bytes=" + std::to_wstring(catalog.total_size) +
                L"; packageCrcVerified=" + (catalog.package_crc_verified ? L"true" : L"false");
        });
        for (const auto& note : catalog.notes) log::write(log::Level::info, L"catalog.note", note);
        return package;
    } catch (const Failure& failure) { log::failure(L"parse.failed", failure); throw; }
}
}
std::unique_ptr<Package> open_package(const fs::path& path) {
    log::Scope step(L"package.detect", &path);
    io::Input input(path);
    const auto bytes = input.bytes();
    constexpr std::array<unsigned char, 8> compound{0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1};
    bool msi = bytes.size() >= compound.size();
    for (std::size_t i = 0; msi && i < compound.size(); ++i) msi = std::to_integer<unsigned char>(bytes[i]) == compound[i];
    if (msi) return parse<formats::MsiPackage>(input.path(), L"MSI");
    if (bytes.size() >= 2 && bytes[0] == std::byte{'M'} && bytes[1] == std::byte{'Z'}) {
        if (formats::burn_probe(bytes)) return parse<formats::BurnPackage>(input.path(), L"WiX Burn");
        if (formats::nsis_probe(bytes)) return parse<formats::NsisPackage>(input.path(), L"NSIS");
        if (formats::archive_probe(bytes)) return parse<formats::ArchivePackage>(input.path(), L"ZIP/7z");
        if (formats::cab_probe(bytes)) return parse<formats::CabPackage>(input.path(), L"CAB");
        return parse<formats::InnoPackage>(input.path(), L"Inno Setup (PE fallback probe)");
    }
    if (formats::cab_probe(bytes)) return parse<formats::CabPackage>(input.path(), L"CAB");
    if (formats::archive_probe(bytes)) return parse<formats::ArchivePackage>(input.path(), L"ZIP/7z");
    throw Failure(Status::unsupported, L"当前支持 MSI、CAB、WiX Burn、NSIS Unicode、受支持版本的 Inno Setup、ZIP/7z 及其 SFX。");
}

void plan_paths(Catalog& catalog) {
    struct NameLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const {
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
        }
    };
    std::vector<bool> conflicts(catalog.files.size(), false);
    std::multimap<std::wstring, std::size_t, NameLess> paths;
    for (std::size_t i = 0; i < catalog.files.size(); ++i) paths.emplace(catalog.files[i].path.wstring(), i);
    for (auto it = paths.begin(); it != paths.end();) {
        const auto end = paths.upper_bound(it->first);
        if (std::next(it) != end) for (auto item = it; item != end; ++item) conflicts[item->second] = true;
        it = end;
    }
    for (std::size_t i = 0; i < catalog.files.size(); ++i) {
        auto parent = catalog.files[i].path.parent_path();
        while (!parent.empty()) {
            const auto [first, last] = paths.equal_range(parent.wstring());
            for (auto it = first; it != last; ++it) conflicts[i] = conflicts[it->second] = true;
            parent = parent.parent_path();
        }
    }
    for (std::size_t i = 0; i < catalog.files.size(); ++i) {
        auto& file = catalog.files[i];
        const auto root = file.path.begin()->wstring();
        if (conflicts[i] || platform::equal_name(root, L"_variants") || platform::equal_name(root, L"_extract-report.json")) {
            constexpr wchar_t hex[] = L"0123456789abcdef";
            std::wstring encoded = L"file-";
            for (wchar_t c : file.id) {
                require(c < 128, Status::internal_error, L"条目稳定 ID 必须使用 ASCII。");
                encoded += hex[(c >> 4) & 15]; encoded += hex[c & 15];
            }
            if (encoded.size() > 255) {
                const auto id = platform::utf8(file.id);
                encoded = L"file-sha256-" + platform::hash_bytes(std::as_bytes(std::span(id)), 32);
            }
            file.path = fs::path(L"_variants") / encoded / file.original_path;
        }
        platform::validate_relative(file.path);
    }
}
} // namespace extract
