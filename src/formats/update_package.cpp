#include "formats/update_package.h"
#include "formats/archive.h"
#include "formats/pe.h"
#include "io/temporary.h"
#include "core/progress.h"
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace extract::formats {
namespace {
const std::array<unsigned char, 32>& signature() {
    // 官方标记为 SHA-256("squirrel bundle")。运行时计算，避免 Extract 自身
    // 的常量区也包含安装器标记，在检查内层普通 EXE 时误识别自身。
    static const auto value = [] {
        const std::string_view seed = "squirrel bundle";
        const auto hex = platform::hash_bytes(std::as_bytes(std::span(seed)), 32);
        std::array<unsigned char, 32> bytes{};
        const auto digit = [](wchar_t c) { return c <= L'9' ? c - L'0' : c - L'a' + 10; };
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<unsigned char>(digit(hex[2 * i]) * 16 + digit(hex[2 * i + 1]));
        return bytes;
    }();
    return value;
}
bool suffix(std::wstring_view value, std::wstring_view tail) {
    return value.size() >= tail.size() && platform::equal_name(value.substr(value.size() - tail.size()), tail);
}
bool release_name(const fs::path& path) {
    const auto name = path.filename().wstring();
    return suffix(name, L"-full.nupkg") || suffix(name, L"-delta.nupkg");
}
struct Container { bool velopack; io::Bytes bytes; };
std::optional<Container> container(io::Bytes bytes, bool probe) {
    if (bytes.size() < 64 || bytes[0] != std::byte{'M'} || bytes[1] != std::byte{'Z'}) return {};
    const auto pe = pe_layout(bytes);
    const auto& bundle_signature = signature();
    std::optional<std::size_t> marker;
    // 只扫描 PE 节，不把内嵌应用或压缩数据里的偶然标记当作外壳。
    for (const auto& section : pe.sections) {
        auto data = io::slice(bytes, section.offset, section.size);
        for (std::size_t start = 0; start < data.size();) {
            control::checkpoint();
            const auto size = (std::min)(data.size() - start, std::size_t{1024 * 1024 + 31});
            const auto part = data.subspan(start, size);
            const auto found = std::search(part.begin(), part.end(), bundle_signature.begin(), bundle_signature.end(),
                [](std::byte a, unsigned char b) { return std::to_integer<unsigned char>(a) == b; });
            if (found != part.end()) {
                const auto offset = section.offset + start + static_cast<std::size_t>(found - part.begin());
                require(!marker, Status::corrupt, L"更新安装包含多个封装标记，无法确定载荷。");
                marker = offset;
                start += static_cast<std::size_t>(found - part.begin()) + bundle_signature.size();
            } else if (size == data.size() - start) break;
            else start += 1024 * 1024;
        }
    }
    if (marker) {
        if (probe) return Container{true, {}};
        require(*marker >= 16, Status::corrupt, L"Velopack 封装头截断。");
        io::Reader header(io::slice(bytes, *marker - 16, 16));
        const auto offset = header.u64(), length = header.u64();
        require(offset && length, Status::unsupported, L"Velopack 外壳没有内嵌完整载荷；请提供离线安装包或 full.nupkg。");
        require(offset >= pe.overlay, Status::corrupt, L"Velopack 载荷覆盖 PE 节。");
        const auto payload = io::slice(bytes, offset, length);
        require(!pe.certificate_offset || offset + length <= pe.certificate_offset, Status::corrupt, L"Velopack 载荷与 PE 签名重叠。");
        return Container{true, payload};
    }
    try { return Container{false, pe_named_resource(bytes, L"DATA", 131)}; }
    catch (const Failure& failure) { if (failure.status != Status::unsupported) throw; }
    return {};
}
void xml_ok(HRESULT value) { require(SUCCEEDED(value), Status::corrupt, L"更新包 nuspec 清单 XML 损坏或含不支持的实体。"); }
std::map<std::wstring, std::wstring> manifest(io::Bytes bytes) {
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size())));
    require(stream != nullptr, Status::limit_exceeded, L"无法分配更新包清单流。");
    Microsoft::WRL::ComPtr<IXmlReader> reader;
    xml_ok(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr));
    xml_ok(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit));
    xml_ok(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 32));
    xml_ok(reader->SetInput(stream.Get()));
    std::map<std::wstring, std::wstring> fields;
    std::vector<std::wstring> parents;
    bool root = false, metadata = false;
    std::wstring ns, active;
    XmlNodeType type; HRESULT hr;
    while ((hr = reader->Read(&type)) == S_OK) {
        control::checkpoint();
        UINT depth = 0, length = 0; const wchar_t* text = nullptr;
        xml_ok(reader->GetDepth(&depth));
        if (type == XmlNodeType_Element) {
            require(active.empty(), Status::corrupt, L"nuspec 标量字段不能包含子元素。");
            xml_ok(reader->GetLocalName(&text, &length)); const std::wstring name(text, length);
            xml_ok(reader->GetNamespaceUri(&text, &length)); const std::wstring element_ns(text, length);
            require(depth <= parents.size(), Status::corrupt, L"nuspec 清单层级无效。");
            if (depth == 0) {
                require(!root && name == L"package", Status::corrupt, L"输入不是 NuGet package 清单。");
                root = true; ns = element_ns;
            }
            if (depth == 1 && name == L"metadata") {
                require(!metadata && element_ns == ns, Status::corrupt, L"nuspec metadata 重复或命名空间不一致。"); metadata = true;
            }
            active.clear();
            if (depth == 2 && parents[1] == L"metadata" &&
                (name == L"id" || name == L"version" || name == L"os" || name == L"mainExe")) {
                require(element_ns == ns && fields.emplace(name, L"").second, Status::corrupt, L"nuspec 字段重复或命名空间不一致。");
                active = name;
            }
            parents.resize(depth); parents.push_back(name);
            if (reader->IsEmptyElement()) active.clear();
        } else if (type == XmlNodeType_EndElement) active.clear();
        else if (!active.empty() && (type == XmlNodeType_Text || type == XmlNodeType_CDATA || type == XmlNodeType_Whitespace)) {
            require(depth == 3, Status::corrupt, L"nuspec 字段内容层级无效。");
            xml_ok(reader->GetValue(&text, &length)); auto& value = fields[active];
            require(length <= 32768 - value.size(), Status::limit_exceeded, L"nuspec 字段过长。"); value.append(text, length);
        }
    }
    xml_ok(hr);
    for (auto& [key, value] : fields) {
        const auto first = value.find_first_not_of(L" \r\n\t");
        value = first == value.npos ? L"" : value.substr(first, value.find_last_not_of(L" \r\n\t") - first + 1);
    }
    require(root && metadata && !fields[L"id"].empty() && !fields[L"version"].empty(), Status::corrupt, L"nuspec 缺少 package/metadata/id/version。");
    return fields;
}
std::wstring releases_hash(io::Bytes data, const Entry& package) {
    std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    std::istringstream lines(text); std::string line, digest;
    const auto wanted = platform::utf8(package.original_path.generic_wstring());
    while (std::getline(lines, line)) {
        if (const auto comment = line.find('#'); comment != line.npos) line.resize(comment);
        std::istringstream parts(line); std::string hash, name, size, extra;
        if (!(parts >> hash)) continue;
        require(static_cast<bool>(parts >> name >> size) && !(parts >> extra), Status::corrupt, L"Squirrel RELEASES 记录无效。");
        if (name != wanted) continue;
        require(digest.empty() && hash.size() == 40 && std::all_of(hash.begin(), hash.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }), Status::corrupt, L"Squirrel RELEASES 摘要无效或记录重复。");
        std::uint64_t count = 0;
        for (auto c : size) {
            require(c >= '0' && c <= '9' && count <= (max_file_bytes - (c - '0')) / 10, Status::corrupt, L"Squirrel RELEASES 大小溢出。");
            count = count * 10 + (c - '0');
        }
        require(count == package.size, Status::corrupt, L"Squirrel RELEASES 与内嵌包大小不符。"); digest = hash;
    }
    require(!digest.empty(), Status::corrupt, L"Squirrel RELEASES 未列出内嵌完整包。");
    return std::wstring(digest.begin(), digest.end());
}
}
bool update_package_probe(const fs::path& path, io::Bytes bytes) {
    return release_name(path) || container(bytes, true).has_value();
}
struct UpdatePackage::Impl {
    io::Input input;
    std::unique_ptr<io::TemporaryFile> cache;
    std::unique_ptr<ArchivePackage> archive;
    explicit Impl(const fs::path& path) : input(path) {
        auto data = input.bytes();
        const auto location = container(data, false);
        std::wstring format = L"Squirrel release package";
        std::vector<std::wstring> notes;
        if (location) {
            data = location->bytes;
            format = location->velopack ? L"Velopack" : L"Squirrel.Windows";
        }
        archive = std::make_unique<ArchivePackage>(input.path(), data);
        if (location && !location->velopack) {
            const auto& files = archive->catalog().files;
            std::optional<std::size_t> full, releases;
            bool updater = false, delta = false;
            for (std::size_t i = 0; i < files.size(); ++i) {
                const auto& name = files[i].original_path;
                if (name.has_parent_path()) continue;
                if (platform::equal_name(name.native(), L"Update.exe")) updater = true;
                if (platform::equal_name(name.native(), L"RELEASES")) {
                    require(!releases, Status::corrupt, L"Squirrel RELEASES 重复。"); releases = i;
                }
                if (suffix(name.native(), L"-delta.nupkg")) delta = true;
                if (suffix(name.native(), L"-full.nupkg")) {
                    require(!full, Status::unsupported, L"Squirrel 封装包含多个完整包，尚不支持自动选择版本。"); full = i;
                }
            }
            require(full.has_value(), Status::unsupported, delta ? L"Squirrel 只包含差分更新；缺少旧版本基础包，请提供 full.nupkg。" : L"Squirrel 外壳没有完整离线载荷，请提供 full.nupkg。");
            require(updater && releases.has_value(), Status::corrupt, L"Squirrel 外壳缺少 Update.exe 或 RELEASES。");
            const auto expected = releases_hash(archive->read_metadata(*releases, 4 * 1024 * 1024), files[*full]);
            progress::Scope decoding(progress::Phase::decoding, {}, L"Squirrel 内嵌完整包");
            platform::ensure_disk_space(io::temporary_directory(), files[*full].size);
            cache = std::make_unique<io::TemporaryFile>(L"squirrel-package");
            archive->read_member(*full, [&](io::Bytes part) { cache->append(part); });
            data = cache->bytes();
            require(platform::equal_name(platform::hash_bytes(data, 20), expected), Status::corrupt, L"Squirrel 内嵌包 SHA-1 与 RELEASES 不符。");
            archive.reset(); input.unmap();
            archive = std::make_unique<ArchivePackage>(input.path(), data);
            notes.push_back(L"已校验 DATA/131 中完整包的 ZIP CRC-32、RELEASES 大小与 SHA-1；启动器及外层更新脚本不执行。");
        }
        const auto& files = archive->catalog().files;
        std::optional<std::size_t> spec;
        std::set<std::wstring> frameworks;
        bool delta = suffix(path.filename().native(), L"-delta.nupkg");
        std::vector<std::pair<std::wstring, fs::path>> app_paths(files.size());
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto& original = files[i].original_path;
            if (!original.has_parent_path() && suffix(original.native(), L".nuspec")) {
                require(!spec, Status::corrupt, L"更新包根目录包含多个 nuspec 清单。"); spec = i;
            }
            auto it = original.begin();
            if (it == original.end() || !platform::equal_name(it->native(), L"lib")) continue;
            if (++it == original.end()) continue;
            auto framework = it->wstring(); fs::path relative;
            for (++it; it != original.end(); ++it) relative /= *it;
            if (relative.empty()) continue;
            if (suffix(relative.native(), L".diff") || suffix(relative.native(), L".bsdiff") || suffix(relative.native(), L".zsdiff")) delta = true;
            require(!suffix(relative.native(), L".__symlink"), Status::unsupported, L"更新包包含需要还原的符号链接；当前只提取普通文件。");
            frameworks.insert(framework); app_paths[i] = {framework, relative};
        }
        require(!delta, Status::unsupported, L"这是差分更新包，缺少旧版本基础文件；请提供完整离线安装包或 full.nupkg。");
        require(spec.has_value() && !frameworks.empty(), Status::unsupported, L"未发现完整更新包所需的根 nuspec 清单及 lib/<目标>/ 应用文件。");
        const auto fields = manifest(archive->read_metadata(*spec, 4 * 1024 * 1024));
        const auto os = fields.find(L"os");
        require(os == fields.end() || os->second.empty() || os->second == L"win" || os->second == L"windows", Status::unsupported, L"此更新包的目标系统不是 Windows。");
        if (!location && os != fields.end() && fields.contains(L"mainExe")) format = L"Velopack";
        const bool velopack = format == L"Velopack";
        if (const auto main = fields.find(L"mainExe"); main != fields.end() && !main->second.empty()) {
            const fs::path executable(main->second); platform::validate_relative(executable);
            require(std::any_of(app_paths.begin(), app_paths.end(), [&](const auto& entry) {
                return platform::equal_name(entry.second.generic_wstring(), executable.generic_wstring());
            }), Status::corrupt, L"更新包缺少 nuspec 声明的主程序：" + main->second);
        }
        std::vector<fs::path> paths;
        bool sq_version = false;
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto& [framework, relative] = app_paths[i];
            const bool auxiliary = suffix(relative.native(), L"_ExecutionStub.exe") || (velopack && platform::equal_name(relative.native(), L"Squirrel.exe"));
            if (!relative.empty() && !auxiliary) {
                auto target = fs::path(L"app");
                if (frameworks.size() > 1) target /= framework;
                target /= relative; paths.push_back(target);
                if (platform::equal_name(relative.native(), L"sq.version")) sq_version = true;
            } else paths.push_back(fs::path(L"_package") / files[i].original_path);
        }
        if (velopack && !sq_version && frameworks.size() == 1) paths[*spec] = fs::path(L"app") / L"sq.version";
        if (frameworks.size() > 1) notes.push_back(L"存在多个目标框架，分别保存在 app/<目标>/，不覆盖同名文件。请选择适用目录。");
        notes.push_back(L"完整包静态展开：应用位于 app，更新辅助文件与打包元数据位于 _package；不运行安装、更新或首次启动钩子。是否可直接运行仍取决于应用的系统依赖。");
        archive->set_layout(std::move(format), fields.at(L"version"), paths, std::move(notes));
    }
};
UpdatePackage::UpdatePackage(const fs::path& path) : impl_(std::make_unique<Impl>(path)) {}
UpdatePackage::~UpdatePackage() = default;
const Catalog& UpdatePackage::catalog() const { return impl_->archive->catalog(); }
fs::path UpdatePackage::extract(const fs::path& parent) { return impl_->archive->extract(parent); }
}
