#include "formats/burn.h"
#include "platform/log.h"
#include "formats/pe.h"
#include "codecs/cab.h"
#include <objbase.h>
#include <objidl.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <set>

namespace extract::formats {
namespace {
using Attributes = std::map<std::wstring, std::wstring>;
struct Manifest { std::vector<Attributes> ux, containers, payloads; std::wstring xml_namespace; };
void xml_ok(HRESULT hr) { require(SUCCEEDED(hr), Status::corrupt, L"Burn 清单 XML 损坏或包含不支持的实体。"); }
const std::wstring& attribute(const Attributes& attributes, const wchar_t* name) {
    const auto it = attributes.find(name);
    require(it != attributes.end() && !it->second.empty(), Status::corrupt, L"Burn 清单缺少字段：" + std::wstring(name));
    return it->second;
}
std::wstring optional(const Attributes& attributes, const wchar_t* name) {
    auto it = attributes.find(name); return it == attributes.end() ? L"" : it->second;
}
std::uint64_t number(const std::wstring& text) {
    require(!text.empty(), Status::corrupt, L"Burn 数字字段为空。");
    std::uint64_t result = 0;
    for (auto c : text) {
        require(c >= L'0' && c <= L'9' && result <= (UINT64_MAX - static_cast<unsigned>(c - L'0')) / 10,
                Status::corrupt, L"Burn 数字字段溢出或格式无效。");
        result = result * 10 + static_cast<unsigned>(c - L'0');
    }
    return result;
}
void hash_value(const std::wstring& hash) {
    require(hash.size() == 40 || hash.size() == 64 || hash.size() == 128, Status::unsupported, L"不支持此 Burn 哈希算法。");
    for (auto c : hash) require((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'),
                              Status::corrupt, L"Burn 哈希编码无效。");
}
Manifest read_manifest(io::Bytes bytes) {
    require(bytes.size() <= 8 * 1024 * 1024, Status::limit_exceeded, L"Burn 清单超过 8 MiB。");
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size())));
    require(stream != nullptr, Status::limit_exceeded, L"无法分配 Burn 清单流。");
    Microsoft::WRL::ComPtr<IXmlReader> reader;
    xml_ok(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr));
    xml_ok(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit));
    xml_ok(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 64));
    xml_ok(reader->SetInput(stream.Get()));
    Manifest result;
    std::vector<std::wstring> parents;
    std::size_t nodes = 0, chars = 0;
    XmlNodeType type; HRESULT hr;
    while ((hr = reader->Read(&type)) == S_OK) {
        require(++nodes <= 100000, Status::limit_exceeded, L"Burn XML 节点过多。");
        if (type != XmlNodeType_Element) continue;
        UINT depth = 0; xml_ok(reader->GetDepth(&depth));
        require(depth <= parents.size() && depth <= 64, Status::corrupt, L"Burn XML 层级异常。");
        const wchar_t* text = nullptr; UINT length = 0;
        xml_ok(reader->GetLocalName(&text, &length)); const std::wstring name(text, length);
        xml_ok(reader->GetNamespaceUri(&text, &length)); const std::wstring ns(text, length);
        if (!depth) {
            require(result.xml_namespace.empty() && name == L"BurnManifest", Status::corrupt, L"输入不是 Burn 清单。");
            require(ns == L"http://schemas.microsoft.com/wix/2008/Burn" || ns == L"http://wixtoolset.org/schemas/v4/2008/Burn",
                    Status::unsupported, L"不支持此 Burn 清单命名空间。");
            result.xml_namespace = ns;
        }
        const bool ux = depth == 2 && parents[1] == L"UX" && name == L"Payload";
        const bool container = depth == 1 && name == L"Container";
        const bool payload = depth == 1 && name == L"Payload";
        if (ux || container || payload) {
            require(ns == result.xml_namespace, Status::corrupt, L"Burn 清单字段命名空间不符。");
            Attributes attrs;
            auto moved = reader->MoveToFirstAttribute();
            while (moved == S_OK) {
                xml_ok(reader->GetLocalName(&text, &length)); std::wstring key(text, length);
                xml_ok(reader->GetValue(&text, &length));
                require(length <= 32767 && chars + length <= 8 * 1024 * 1024 && attrs.size() < 64,
                        Status::limit_exceeded, L"Burn XML 属性超过预算。");
                chars += length;
                require(attrs.emplace(std::move(key), std::wstring(text, length)).second, Status::corrupt, L"Burn XML 属性重复。");
                moved = reader->MoveToNextAttribute();
            }
            xml_ok(moved); xml_ok(reader->MoveToElement());
            auto& list = ux ? result.ux : (container ? result.containers : result.payloads);
            list.push_back(std::move(attrs));
        }
        parents.resize(depth); parents.push_back(name);
    }
    xml_ok(hr); require(!result.xml_namespace.empty(), Status::corrupt, L"Burn 清单为空。");
    return result;
}
std::vector<io::Bytes> attached(io::Bytes bytes) {
    const auto pe = pe_layout(bytes);
    const PeSection* section = nullptr;
    for (const auto& s : pe.sections) if (s.name == ".wixburn") {
        require(!section, Status::corrupt, L"Burn 节重复。"); section = &s;
    }
    require(section != nullptr, Status::unsupported, L"没有 Burn 节。");
    io::Reader r(io::slice(bytes, section->offset, section->size));
    require(r.u32() == 0x00f14300, Status::corrupt, L"Burn 节标识损坏。");
    require(r.u32() == 2, Status::unsupported, L"当前支持 Burn 二进制布局版本 2。");
    r.skip(16); const auto stub = r.u32(); r.skip(4);
    const auto original_signature = r.u32(), original_size = r.u32();
    require(r.u32() == 1, Status::unsupported, L"Burn 使用了非 CAB 容器。");
    const auto count = r.u32();
    require(count > 0 && count <= 128, Status::limit_exceeded, L"Burn 容器数量无效或超限。");
    require(stub >= pe.overlay, Status::corrupt, L"Burn stub 与 PE 节重叠。");
    std::vector<std::uint32_t> sizes; for (std::uint32_t i = 0; i < count; ++i) sizes.push_back(r.u32());
    std::vector<io::Bytes> result{io::slice(bytes, stub, sizes[0])};
    std::uint64_t cursor = static_cast<std::uint64_t>(stub) + sizes[0];
    require((original_signature == 0) == (original_size == 0), Status::corrupt, L"Burn 原始签名信息无效。");
    if (original_signature) {
        require(original_signature >= cursor, Status::corrupt, L"Burn 原始签名覆盖 UX 容器。");
        (void)io::slice(bytes, original_signature, original_size);
        cursor = static_cast<std::uint64_t>(original_signature) + original_size;
    }
    for (std::size_t i = 1; i < sizes.size(); ++i) {
        result.push_back(io::slice(bytes, cursor, sizes[i])); cursor += sizes[i];
    }
    // Engine-only signed files may point at their original signature; complete bundles sign after all containers.
    if (pe.certificate_offset) require(pe.certificate_offset >= cursor ||
        (pe.certificate_offset == original_signature && pe.certificate_size == original_size),
        Status::corrupt, L"Burn 容器与 PE 签名重叠。");
    return result;
}
}
bool burn_probe(io::Bytes bytes) {
    if (bytes.size() < 64 || bytes[0] != std::byte{'M'} || bytes[1] != std::byte{'Z'}) return false;
    const auto pe = pe_layout(bytes);
    return std::any_of(pe.sections.begin(), pe.sections.end(), [](const PeSection& s) { return s.name == ".wixburn"; });
}

struct BurnPackage::Impl {
    io::Input input;
    Catalog catalog;
    struct Container { io::Bytes bytes; std::vector<codecs::CabMember> members; std::vector<std::size_t> targets; };
    std::vector<Container> containers;
    std::vector<std::unique_ptr<io::Input>> external_inputs;
    struct Loose { io::Bytes bytes; std::size_t entry; };
    std::vector<Loose> loose;
    std::size_t member_name_bytes = 0;
    static constexpr std::size_t missing = SIZE_MAX;

    io::Input* external(const fs::path& relative) {
        platform::validate_relative(relative);
        const auto path = input.path().parent_path() / relative;
        const auto attrs = GetFileAttributesW(platform::extended_path(path).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
                catalog.content_complete = false;
                catalog.notes.push_back(L"缺少外置或在线载荷：" + relative.wstring());
                log::detail(log::Level::warning, L"burn.payload_missing", [&] { return path.wstring(); });
                return nullptr;
            }
            platform::io_failure(L"无法检查 Burn 外置载荷：" + relative.wstring(), code);
        }
        require(external_inputs.size() < 128, Status::limit_exceeded, L"Burn 外置文件数量超过 128。");
        auto value = std::make_unique<io::Input>(path);
        auto* result = value.get(); external_inputs.push_back(std::move(value)); return result;
    }
    std::size_t add_entry(const fs::path& path, std::uint64_t size, const std::wstring& source, const std::wstring& hash = {}) {
        platform::validate_relative(path);
        require(size <= max_file_bytes - catalog.total_size, Status::limit_exceeded, L"Burn 输出预算超限。");
        Entry entry; entry.id = L"burn-" + std::to_wstring(catalog.files.size());
        entry.path = entry.original_path = path; entry.size = size; entry.source_expression = source;
        if (!hash.empty()) {
            hash_value(hash);
            if (hash.size() == 40) entry.expected_sha1 = hash;
            else if (hash.size() == 64) entry.expected_sha256 = hash;
            else entry.expected_sha512 = hash;
        }
        catalog.total_size += size; catalog.files.push_back(std::move(entry)); return catalog.files.size() - 1;
    }
    std::size_t add_container(io::Bytes bytes) {
        require(containers.size() < 128, Status::limit_exceeded, L"Burn 容器数量超过 128。");
        auto members = codecs::cab_members(bytes);
        const auto size = members.size();
        for (const auto& member : members) {
            require(member.name.size() <= 8 * 1024 * 1024 - member_name_bytes, Status::limit_exceeded, L"Burn CAB 文件名元数据超过 8 MiB。");
            member_name_bytes += member.name.size();
        }
        containers.push_back({bytes, std::move(members), std::vector<std::size_t>(size, missing)});
        return containers.size() - 1;
    }
    void map_member(std::size_t container, const std::wstring& source, const fs::path& path,
                    std::optional<std::uint64_t> size = {}, const std::wstring& hash = {}) {
        auto& c = containers.at(container);
        std::optional<std::size_t> found;
        for (std::size_t i = 0; i < c.members.size(); ++i) if (c.members[i].decoded_name == source) {
            require(!found, Status::corrupt, L"Burn CAB 内部编号重复。"); found = i;
        }
        require(found.has_value(), Status::corrupt, L"Burn 清单引用了缺失的 CAB 成员：" + source);
        require(c.targets[*found] == missing, Status::corrupt, L"Burn 清单重复映射 CAB 成员。");
        require(!size || *size == c.members[*found].size, Status::corrupt, L"Burn 载荷大小与 CAB 不符。");
        c.targets[*found] = add_entry(path, c.members[*found].size, source, hash);
    }
    explicit Impl(const fs::path& path) : input(path) {
        catalog.input = input.path(); catalog.format = L"WiX Burn"; catalog.format_version = L"2";
        const auto slots = attached(input.bytes());
        add_container(slots[0]);
        const auto& members = containers[0].members;
        auto manifest_member = std::find_if(members.begin(), members.end(), [](const auto& m) { return m.name == "0"; });
        require(manifest_member != members.end(), Status::corrupt, L"Burn UX 容器缺少清单成员 0。");
        const auto manifest_bytes = codecs::cab_member_bytes(slots[0], static_cast<std::size_t>(manifest_member - members.begin()), 8 * 1024 * 1024);
        log::write(log::Level::info, L"burn.manifest_parse");
        const auto manifest = read_manifest(manifest_bytes);
        map_member(0, L"0", L"bootstrapper/manifest.xml");
        for (const auto& item : manifest.ux) {
            fs::path file = attribute(item, L"FilePath"); platform::validate_relative(file);
            map_member(0, attribute(item, L"SourcePath"), fs::path(L"bootstrapper") / file);
        }
        std::map<std::wstring, std::size_t> names;
        std::set<std::uint64_t> used_slots{0};
        for (const auto& item : manifest.containers) {
            const auto& id = attribute(item, L"Id"); platform::validate_component(id);
            log::write(log::Level::info, L"burn.container_parse", id);
            require(!names.contains(id), Status::corrupt, L"Burn 容器 ID 重复。");
            io::Bytes bytes; bool present = true;
            const auto attached_flag = optional(item, L"Attached");
            require(attached_flag.empty() || attached_flag == L"yes" || attached_flag == L"no", Status::corrupt, L"Burn Attached 标志无效。");
            if (attached_flag == L"yes") {
                const auto index = number(attribute(item, L"AttachedIndex"));
                require(index > 0 && index < slots.size() && used_slots.insert(index).second, Status::corrupt, L"Burn 容器索引无效。");
                bytes = slots[static_cast<std::size_t>(index)];
            } else {
                auto* file = external(attribute(item, L"FilePath")); present = file != nullptr;
                if (file) bytes = file->bytes();
            }
            const auto expected_size = number(attribute(item, L"FileSize"));
            const auto& hash = attribute(item, L"Hash"); hash_value(hash);
            if (present) {
                require(bytes.size() == expected_size, Status::corrupt, L"Burn 容器大小不符。");
                require(platform::equal_name(platform::hash_bytes(bytes, hash.size() / 2), hash), Status::corrupt, L"Burn 容器哈希不符。");
                log::write(log::Level::info, L"burn.container_verified", id);
                names.emplace(id, add_container(bytes));
            } else names.emplace(id, missing);
        }
        require(used_slots.size() == slots.size(), Status::corrupt, L"Burn 存在未在清单声明的附加容器。");
        std::set<std::wstring> payload_ids;
        for (const auto& item : manifest.payloads) {
            require(payload_ids.insert(attribute(item, L"Id")).second, Status::corrupt, L"Burn 载荷 ID 重复。");
            const auto& source = attribute(item, L"SourcePath");
            const auto& file = attribute(item, L"FilePath"); platform::validate_relative(file);
            const auto size = number(attribute(item, L"FileSize")); const auto hash = optional(item, L"Hash");
            if (!hash.empty()) hash_value(hash);
            else catalog.notes.push_back(L"未执行证书信任校验，载荷仅记录提取 SHA-256：" + file);
            const auto& packaging = attribute(item, L"Packaging");
            if (packaging == L"embedded") {
                auto found = names.find(attribute(item, L"Container"));
                require(found != names.end(), Status::corrupt, L"Burn 载荷引用不存在的容器。");
                if (found->second != missing) map_member(found->second, source, fs::path(L"packages") / found->first / file, size, hash);
            } else if (packaging == L"external") {
                if (auto* data = external(source)) {
                    require(data->bytes().size() == size, Status::corrupt, L"Burn 外置载荷大小不符。");
                    loose.push_back({data->bytes(), add_entry(fs::path(L"packages") / L"external" / file, size, source, hash)});
                }
            } else throw Failure(Status::unsupported, L"不支持此 Burn 载荷封装方式。");
        }
        for (const auto& c : containers) for (auto index : c.targets)
            require(index != missing, Status::corrupt, L"Burn CAB 存在未在清单映射的成员。");
        catalog.notes.push_back(L"静态提取 Burn；引导程序位于 bootstrapper，安装载荷位于 packages。未运行安装链或下载文件。");
        plan_paths(catalog);
    }
};
BurnPackage::BurnPackage(const fs::path& path) : impl_(std::make_unique<Impl>(path)) {}
BurnPackage::~BurnPackage() = default;
const Catalog& BurnPackage::catalog() const { return impl_->catalog; }
fs::path BurnPackage::extract(const fs::path& parent) {
    auto& catalog = impl_->catalog;
    io::Output output(parent.empty() ? catalog.input.parent_path() : parent, catalog.input.stem().wstring());
    for (const auto& c : impl_->containers) {
        log::Scope step(L"burn.container");
        log::detail(log::Level::info, L"burn.container_begin", [&] {
            return L"index=" + std::to_wstring(&c - impl_->containers.data()) + L"; files=" + std::to_wstring(c.targets.size()) + L"; bytes=" + std::to_wstring(c.bytes.size());
        });
        std::vector<Entry*> targets; for (auto index : c.targets) targets.push_back(&catalog.files[index]);
        codecs::extract_cab(c.bytes, targets, output);
    }
    for (const auto& source : impl_->loose) {
        auto& entry = catalog.files[source.entry];
        log::Scope step(L"burn.loose_file", nullptr, &entry.path);
        { auto file = output.create_file(entry.path); platform::write_all(file.get(), source.bytes);
          if (!FlushFileBuffers(file.get())) platform::io_failure(L"保存 Burn 外置载荷失败"); }
        codecs::verify_file(entry, output.full_path(entry.path));
    }
    const auto report = platform::utf8(catalog_json(catalog, true));
    { auto file = output.create_file(L"_extract-report.json"); platform::write_all(file.get(), std::as_bytes(std::span(report)));
      if (!FlushFileBuffers(file.get())) platform::io_failure(L"保存 Burn 报告失败"); }
    return output.commit();
}
}
