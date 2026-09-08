#include "formats/msix.h"
#include "formats/archive.h"
#include "core/control.h"
#include <bcrypt.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <set>

namespace extract::formats {
namespace {
constexpr std::size_t xml_limit = 16 * 1024 * 1024;
constexpr std::wstring_view block_ns = L"http://schemas.microsoft.com/appx/2010/blockmap";
constexpr std::wstring_view bundle_ns = L"http://schemas.microsoft.com/appx/2013/bundle";
constexpr std::wstring_view bundle_manifest = L"AppxMetadata/AppxBundleManifest.xml";
struct NameLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
using Index = std::map<std::wstring, std::size_t, NameLess>;
using Attributes = std::map<std::wstring, std::wstring>;
std::wstring get(const Attributes& a, const wchar_t* key, std::wstring fallback = {}) {
    const auto it = a.find(key); return it == a.end() ? fallback : it->second;
}
std::wstring required(const Attributes& a, const wchar_t* key) {
    auto value = get(a, key);
    require(!value.empty(), Status::corrupt, L"MSIX 清单缺少字段：" + std::wstring(key)); return value;
}
std::uint64_t number(std::wstring_view text) {
    require(!text.empty(), Status::corrupt, L"MSIX 数字字段为空。");
    std::uint64_t result = 0;
    for (auto c : text) {
        require(c >= L'0' && c <= L'9' && result <= (max_file_bytes - (c - L'0')) / 10, Status::corrupt, L"MSIX 数字字段无效或溢出。");
        result = result * 10 + (c - L'0');
    }
    return result;
}
bool boolean(std::wstring_view text) {
    require(text == L"true" || text == L"false" || text == L"1" || text == L"0", Status::corrupt, L"MSIX 布尔字段无效。");
    return text == L"true" || text == L"1";
}
void version(std::wstring_view text) {
    for (unsigned i = 0; i < 4; ++i) {
        const auto end = text.find(L'.');
        require((i == 3) == (end == text.npos), Status::corrupt, L"MSIX 版本必须由四段数字组成。");
        require(number(text.substr(0, end)) <= 65535, Status::corrupt, L"MSIX 版本字段超出范围。");
        if (end != text.npos) text.remove_prefix(end + 1);
    }
}
void architecture(std::wstring_view text) {
    require(text == L"x86" || text == L"x64" || text == L"arm" || text == L"arm64" || text == L"neutral",
            Status::unsupported, L"暂不支持此 MSIX 架构：" + std::wstring(text));
}
std::wstring trim(std::wstring text) {
    const auto first = text.find_first_not_of(L" \r\n\t");
    return first == text.npos ? L"" : text.substr(first, text.find_last_not_of(L" \r\n\t") - first + 1);
}
void xml_ok(HRESULT hr) { require(SUCCEEDED(hr), Status::corrupt, L"MSIX XML 损坏或包含不支持的实体。"); }
struct Node {
    XmlNodeType type;
    unsigned depth;
    std::wstring name, ns, value;
    Attributes attributes;
};
void xml(io::Bytes bytes, const std::function<void(const Node&, const std::vector<std::wstring>&)>& visit) {
    Microsoft::WRL::ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size())));
    require(stream != nullptr, Status::limit_exceeded, L"无法分配 MSIX XML 流。");
    Microsoft::WRL::ComPtr<IXmlReader> reader;
    xml_ok(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr));
    xml_ok(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit));
    xml_ok(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 64)); xml_ok(reader->SetInput(stream.Get()));
    Node node{}; HRESULT hr; std::vector<std::wstring> path; std::size_t count = 0;
    while ((hr = reader->Read(&node.type)) == S_OK) {
        control::checkpoint(); require(++count <= 500000, Status::limit_exceeded, L"MSIX XML 节点超过元数据预算。");
        UINT depth = 0, length = 0; const wchar_t* text = nullptr;
        xml_ok(reader->GetDepth(&depth)); node.depth = depth; node.attributes.clear(); node.value.clear();
        if (node.type == XmlNodeType_Element) {
            xml_ok(reader->GetLocalName(&text, &length)); node.name.assign(text, length);
            xml_ok(reader->GetNamespaceUri(&text, &length)); node.ns.assign(text, length);
            require(depth <= path.size(), Status::corrupt, L"MSIX XML 层级错误。"); path.resize(depth); path.push_back(node.name);
            auto moved = reader->MoveToFirstAttribute();
            while (moved == S_OK) {
                xml_ok(reader->GetNamespaceUri(&text, &length));
                if (!length) {
                    xml_ok(reader->GetLocalName(&text, &length)); std::wstring key(text, length);
                    xml_ok(reader->GetValue(&text, &length));
                    require(length <= 32768 && node.attributes.size() < 128, Status::limit_exceeded, L"MSIX XML 属性超限。");
                    require(node.attributes.emplace(key, std::wstring(text, length)).second, Status::corrupt, L"MSIX XML 属性重复。");
                }
                moved = reader->MoveToNextAttribute();
            }
            xml_ok(moved); if (reader->GetNodeType(&node.type) == S_OK && node.type == XmlNodeType_Attribute) xml_ok(reader->MoveToElement());
            node.type = XmlNodeType_Element;
        } else if (node.type == XmlNodeType_Text || node.type == XmlNodeType_CDATA || node.type == XmlNodeType_Whitespace) {
            xml_ok(reader->GetValue(&text, &length)); node.value.assign(text, length);
        }
        visit(node, path);
    }
    xml_ok(hr);
}
std::wstring utf8_text(const std::string& bytes) {
    if (bytes.empty()) return {};
    const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    require(n > 0, Status::unsafe_path, L"MSIX OPC 路径 UTF-8 无效。");
    std::wstring result(n, L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), result.data(), n) == n,
            Status::unsafe_path, L"无法转换 MSIX 路径。"); return result;
}
fs::path opc_path(const fs::path& path) {
    auto encoded = platform::utf8(path.generic_wstring()); std::string decoded;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1;
    };
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        char c = encoded[i];
        if (c == '%') {
            require(i + 2 < encoded.size() && digit(encoded[i + 1]) >= 0 && digit(encoded[i + 2]) >= 0, Status::unsafe_path, L"MSIX OPC 百分号编码无效。");
            c = static_cast<char>(digit(encoded[i + 1]) * 16 + digit(encoded[i + 2])); i += 2;
            require(c != '/' && c != '\\', Status::unsafe_path, L"MSIX OPC 路径包含编码后的分隔符。");
        }
        decoded += c;
    }
    fs::path result(utf8_text(decoded)); platform::validate_relative(result); return result;
}
Index index_files(const ArchivePackage& archive, std::vector<fs::path>& paths) {
    Index result;
    for (const auto& file : archive.catalog().files) {
        const auto decoded = opc_path(file.original_path);
        require(result.emplace(decoded.generic_wstring(), paths.size()).second, Status::corrupt, L"MSIX 解码后的文件路径重复。"); paths.push_back(decoded);
    }
    // 不将包中不合法的文件/目录冲突迁移为可运行包的其它目录。
    for (const auto& path : paths) for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path())
        require(!result.contains(parent.generic_wstring()), Status::corrupt, L"MSIX 文件与目录路径冲突。");
    return result;
}
std::size_t find(const Index& index, std::wstring_view name) {
    const auto it = index.find(std::wstring(name));
    require(it != index.end(), Status::corrupt, L"MSIX 缺少文件：" + std::wstring(name)); return it->second;
}
struct Identity {
    std::wstring name, publisher, ver, arch = L"neutral", resource_id;
    bool resource = false, framework = false;
    std::vector<fs::path> executables;
    std::vector<std::wstring> notes;
};
Identity manifest(io::Bytes bytes) {
    Identity result; std::wstring ns, resource, framework; bool root = false, id = false;
    std::set<std::wstring> containers, properties;
    xml(bytes, [&](const Node& n, const auto& p) {
        if (n.type == XmlNodeType_Element) {
            if (!n.depth) {
                require(!root && n.name == L"Package", Status::corrupt, L"输入不是 AppxManifest Package。"); root = true; ns = n.ns;
                require(ns == L"http://schemas.microsoft.com/appx/manifest/foundation/windows10" ||
                        ns == L"http://schemas.microsoft.com/appx/2010/manifest" || ns == L"http://schemas.microsoft.com/appx/2013/manifest",
                        Status::unsupported, L"暂不支持此 AppxManifest 命名空间。");
            }
            if (n.depth == 1 && n.name == L"Identity") {
                require(!id && n.ns == ns, Status::corrupt, L"AppxManifest Identity 重复或命名空间错误。"); id = true;
                result.name = required(n.attributes, L"Name"); result.publisher = required(n.attributes, L"Publisher");
                result.ver = required(n.attributes, L"Version"); version(result.ver);
                result.arch = get(n.attributes, L"ProcessorArchitecture", L"neutral"); architecture(result.arch);
                result.resource_id = get(n.attributes, L"ResourceId");
            }
            if (n.depth == 1 && (n.name == L"Properties" || n.name == L"Applications" || n.name == L"Dependencies"))
                require(n.ns == ns && containers.insert(n.name).second, Status::corrupt, L"AppxManifest 容器重复或命名空间错误。");
            if (n.depth == 2 && p[1] == L"Properties" && (n.name == L"ResourcePackage" || n.name == L"Framework"))
                require(n.ns == ns && properties.insert(n.name).second, Status::corrupt, L"AppxManifest 包属性重复或命名空间错误。");
            if (n.depth == 2 && p[1] == L"Applications" && n.name == L"Application") {
                require(n.ns == ns, Status::corrupt, L"AppxManifest Application 命名空间错误。");
                auto executable = get(n.attributes, L"Executable");
                if (!executable.empty()) { fs::path path(executable); platform::validate_relative(path); result.executables.push_back(path); }
            }
            if (n.depth == 2 && p[1] == L"Dependencies") {
                std::wstring note = L"清单依赖 " + n.name + L"：";
                for (const auto& [key, value] : n.attributes) note += key + L"=" + value + L"; ";
                result.notes.push_back(std::move(note));
            }
        } else if (!n.value.empty() && p.size() == 3 && p[1] == L"Properties") {
            if (p[2] == L"ResourcePackage") resource += n.value;
            if (p[2] == L"Framework") framework += n.value;
        }
    });
    require(root && id, Status::corrupt, L"AppxManifest 缺少包身份。");
    if (properties.contains(L"ResourcePackage")) result.resource = boolean(trim(resource));
    if (properties.contains(L"Framework")) result.framework = boolean(trim(framework));
    return result;
}
std::wstring base64_hash(std::wstring_view text, std::size_t bytes) {
    std::vector<unsigned char> decoded; unsigned accumulator = 0, bits = 0; std::size_t padding = 0, characters = 0;
    constexpr std::wstring_view alphabet = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (auto c : text) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
        ++characters;
        if (c == L'=') { ++padding; continue; }
        const auto digit = alphabet.find(c);
        require(!padding && digit != alphabet.npos, Status::corrupt, L"MSIX 块摘要 Base64 无效。");
        accumulator = (accumulator << 6) | static_cast<unsigned>(digit); bits += 6;
        if (bits >= 8) { bits -= 8; decoded.push_back(static_cast<unsigned char>(accumulator >> bits)); }
    }
    require(decoded.size() == bytes && characters == ((bytes + 2) / 3) * 4 && padding == (3 - bytes % 3) % 3 &&
            (!bits || (accumulator & ((1U << bits) - 1)) == 0), Status::corrupt, L"MSIX 块摘要长度或填充无效。");
    constexpr wchar_t hex[] = L"0123456789abcdef"; std::wstring result;
    for (auto b : decoded) { result += hex[b >> 4]; result += hex[b & 15]; } return result;
}
struct BlockFile { std::vector<std::wstring> hashes; std::wstring full_hash; std::uint64_t compressed = 0; };
struct BlockMap { std::size_t digest_size = 0; std::map<std::size_t, BlockFile> files; };
BlockMap blockmap(io::Bytes bytes, const ArchivePackage& archive, const Index& index, bool bundle) {
    BlockMap result; bool root = false; std::optional<std::size_t> current;
    xml(bytes, [&](const Node& n, const auto&) {
        if (n.type != XmlNodeType_Element) return;
        if (!n.depth) {
            require(!root && n.name == L"BlockMap", Status::corrupt, L"输入不是 AppxBlockMap。"); root = true;
            require(n.ns == block_ns, Status::unsupported, L"暂不支持此 BlockMap 布局（含加密块映射）。");
            const auto method = required(n.attributes, L"HashMethod");
            result.digest_size = method == L"http://www.w3.org/2001/04/xmlenc#sha256" ? 32 :
                (method == L"http://www.w3.org/2001/04/xmldsig-more#sha384" ? 48 : (method == L"http://www.w3.org/2001/04/xmlenc#sha512" ? 64 : 0));
            require(result.digest_size, Status::unsupported, L"暂不支持此 MSIX 块哈希算法。");
        } else if (n.depth == 1 && n.name == L"File" && n.ns == block_ns) {
            fs::path name(required(n.attributes, L"Name")); platform::validate_relative(name);
            const auto idx = find(index, name.generic_wstring());
            require(result.files.emplace(idx, BlockFile{}).second, Status::corrupt, L"MSIX BlockMap 文件重复。"); current = idx;
            const auto& file = archive.catalog().files[idx]; const auto member = archive.zip_member(idx);
            require(number(required(n.attributes, L"Size")) == file.size && number(required(n.attributes, L"LfhSize")) == member.local_header_size,
                    Status::corrupt, L"MSIX BlockMap 大小或本地头长度与 ZIP 不符。");
            require(member.method == codecs::Compression::stored || member.method == codecs::Compression::raw_deflate, Status::unsupported, L"MSIX 只支持无压缩和 Deflate。");
        } else if (n.depth == 2 && current && n.name == L"Block" && n.ns == block_ns) {
            auto& file = result.files.at(*current); const auto& entry = archive.catalog().files[*current];
            require(file.full_hash.empty() && file.hashes.size() < entry.size / 65536 + (entry.size % 65536 != 0), Status::corrupt, L"MSIX 块数量超出文件范围。");
            file.hashes.push_back(base64_hash(required(n.attributes, L"Hash"), result.digest_size));
            const bool compressed = archive.zip_member(*current).method == codecs::Compression::raw_deflate;
            require(compressed == n.attributes.contains(L"Size"), Status::corrupt, L"MSIX 压缩块 Size 属性不符。");
            if (compressed) {
                const auto size = number(required(n.attributes, L"Size")); require(size > 0, Status::corrupt, L"MSIX 压缩块大小为零。");
                file.compressed = checked_size_sum(file.compressed, size);
            }
        } else if (n.depth == 2 && current && n.name == L"FileHash" && (n.ns == L"http://schemas.microsoft.com/appx/2017/blockmap" || n.ns == L"http://schemas.microsoft.com/appx/2021/blockmap")) {
            auto& file = result.files.at(*current); require(file.full_hash.empty(), Status::corrupt, L"MSIX FileHash 重复。");
            file.full_hash = base64_hash(required(n.attributes, L"Hash"), result.digest_size);
        } else throw Failure(Status::unsupported, L"暂不支持此 MSIX BlockMap 元素或层级。");
    });
    require(root, Status::corrupt, L"MSIX BlockMap 为空。");
    for (const auto& [idx, file] : result.files) {
        const auto size = archive.catalog().files[idx].size; const auto member = archive.zip_member(idx);
        require(file.hashes.size() == size / 65536 + (size % 65536 != 0), Status::corrupt, L"MSIX 文件缺少块摘要。");
        if (member.method == codecs::Compression::raw_deflate) {
            require(file.compressed == member.packed.size() || (member.packed.size() >= 2 && file.compressed == member.packed.size() - 2 &&
                    member.packed[member.packed.size() - 2] == std::byte{3} && member.packed.back() == std::byte{}),
                    Status::corrupt, L"MSIX 压缩块总长度与 ZIP 不一致。");
        }
    }
    const auto mandatory = find(index, bundle ? bundle_manifest : L"AppxManifest.xml");
    require(result.files.contains(mandatory), Status::corrupt, L"MSIX 清单未包含在块校验中。");
    for (const auto& [name, idx] : index) {
        const bool footprint = platform::equal_name(name, L"[Content_Types].xml") || platform::equal_name(name, L"AppxBlockMap.xml") ||
            platform::equal_name(name, L"AppxSignature.p7x") || platform::equal_name(name, L"AppxMetadata/CodeIntegrity.cat");
        if (footprint) require(!result.files.contains(idx), Status::corrupt, L"MSIX BlockMap 含不允许的包元文件。");
        else if (!bundle) require(result.files.contains(idx), Status::corrupt, L"MSIX 文件未包含在块校验中：" + name);
    }
    if (bundle) require(result.files.size() == 1, Status::corrupt, L"Bundle 块映射只能覆盖 Bundle 清单，成员包应单独校验。");
    return result;
}
class Digest {
public:
    explicit Digest(std::size_t size) : size_(size) {
        require(BCryptOpenAlgorithmProvider(&algorithm_, size == 32 ? BCRYPT_SHA256_ALGORITHM : (size == 48 ? BCRYPT_SHA384_ALGORITHM : BCRYPT_SHA512_ALGORITHM), nullptr, 0) >= 0,
                Status::internal_error, L"无法初始化 MSIX 文件哈希。");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0); algorithm_ = nullptr; throw Failure(Status::internal_error, L"无法创建 MSIX 文件哈希。");
        }
    }
    ~Digest() { if (hash_) BCryptDestroyHash(hash_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
    void add(io::Bytes bytes) { require(BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) >= 0, Status::internal_error, L"MSIX 文件哈希失败。"); }
    std::wstring finish() {
        std::array<unsigned char, 64> value{};
        require(BCryptFinishHash(hash_, value.data(), static_cast<ULONG>(size_), 0) >= 0, Status::internal_error, L"无法完成 MSIX 文件哈希。");
        constexpr wchar_t hex[] = L"0123456789abcdef"; std::wstring result;
        for (std::size_t i = 0; i < size_; ++i) { result += hex[value[i] >> 4]; result += hex[value[i] & 15]; } return result;
    }
private:
    std::size_t size_; BCRYPT_ALG_HANDLE algorithm_ = nullptr; BCRYPT_HASH_HANDLE hash_ = nullptr;
};
}
bool msix_extension(const fs::path& path) {
    const auto e = path.extension().native();
    return platform::equal_name(e, L".msix") || platform::equal_name(e, L".appx") || platform::equal_name(e, L".msixbundle") ||
        platform::equal_name(e, L".appxbundle") || platform::equal_name(e, L".emsix") || platform::equal_name(e, L".eappx") ||
        platform::equal_name(e, L".emsixbundle") || platform::equal_name(e, L".eappxbundle");
}
bool msix_catalog(const Catalog& catalog) {
    return catalog.format == L"ZIP" && std::any_of(catalog.files.begin(), catalog.files.end(), [](const auto& f) {
        const auto path = f.original_path.generic_wstring();
        return platform::equal_name(path, L"AppxManifest.xml") || platform::equal_name(path, bundle_manifest) || platform::equal_name(path, L"AppxBlockMap.xml");
    });
}
struct MsixPackage::Impl {
    ArchivePackage archive;
    BlockMap map;
    std::unique_ptr<Digest> full_digest;
    explicit Impl(const fs::path& path) : archive(path) {
        require(archive.catalog().format == L"ZIP", Status::unsupported, L"MSIX/APPX 必须使用 ZIP 容器。");
        std::vector<fs::path> paths; const auto index = index_files(archive, paths);
        const bool bundle = index.contains(std::wstring(bundle_manifest));
        require(!(bundle && index.contains(L"AppxManifest.xml")), Status::corrupt, L"MSIX 单包与 Bundle 清单不能同时存在。");
        std::vector<std::wstring> notes, conditions(paths.size()); bool complete = true;
        const auto read = [&](std::wstring_view name) { return archive.read_metadata(find(index, name), xml_limit); };
        // 内容类型是 OPC 元数据，不用于宣称签名可信；但必须是安全、可读取的 XML。
        bool content_root = false;
        xml(read(L"[Content_Types].xml"), [&](const Node& n, const auto&) {
            if (n.type == XmlNodeType_Element && !n.depth) {
                require(!content_root && n.name == L"Types" && n.ns == L"http://schemas.openxmlformats.org/package/2006/content-types", Status::corrupt, L"MSIX 内容类型清单无效。"); content_root = true;
            }
        });
        require(content_root, Status::corrupt, L"MSIX 内容类型清单为空。");
        map = blockmap(read(L"AppxBlockMap.xml"), archive, index, bundle);
        std::wstring package_version;
        if (!bundle) {
            const auto identity = manifest(read(L"AppxManifest.xml")); package_version = identity.ver;
            for (const auto& exe : identity.executables) (void)find(index, exe.generic_wstring());
            notes = identity.notes;
            notes.push_back(L"包身份：" + identity.name + L"; Publisher=" + identity.publisher + L"; Version=" + identity.ver + L"; Architecture=" + identity.arch +
                            L"; ResourceId=" + identity.resource_id + L"; Type=" + (identity.resource ? L"resource" : (identity.framework ? L"framework" : L"application")));
        } else {
            bool root = false, identity_found = false, packages_found = false; Identity identity;
            std::set<std::size_t> members; std::set<std::wstring, NameLess> references;
            std::optional<std::size_t> current_member;
            xml(read(bundle_manifest), [&](const Node& n, const auto& parents) {
                if (n.type != XmlNodeType_Element) return;
                if (!n.depth) {
                    require(!root && n.name == L"Bundle", Status::corrupt, L"Bundle 根元素无效。"); root = true;
                    require(n.ns == bundle_ns, Status::unsupported, L"暂不支持此 Bundle 清单命名空间。");
                } else if (n.depth == 1 && n.name == L"Identity") {
                    require(!identity_found && n.ns == bundle_ns, Status::corrupt, L"Bundle Identity 重复或命名空间错误。"); identity_found = true;
                    identity.name = required(n.attributes, L"Name"); identity.publisher = required(n.attributes, L"Publisher");
                    package_version = required(n.attributes, L"Version"); version(package_version);
                } else if (n.depth == 1 && n.name == L"Packages") {
                    require(!packages_found && n.ns == bundle_ns, Status::corrupt, L"Bundle Packages 重复或命名空间错误。"); packages_found = true;
                } else if (n.depth == 2 && parents[1] == L"Packages" && n.name == L"Package") {
                    current_member.reset();
                    require(identity_found && n.ns == bundle_ns, Status::corrupt, L"Bundle 成员清单层级或命名空间错误。");
                    const fs::path name(required(n.attributes, L"FileName")); platform::validate_relative(name);
                    require(!name.has_parent_path() && references.insert(name.native()).second, Status::corrupt, L"Bundle 成员文件名重复或不在根目录。");
                    const auto kind = get(n.attributes, L"Type", L"resource");
                    require(kind == L"application" || kind == L"resource", Status::unsupported, L"未知 Bundle 成员类型。");
                    const auto arch = get(n.attributes, L"Architecture", L"neutral"); architecture(arch);
                    const auto ver = required(n.attributes, L"Version"); version(ver);
                    const auto resource_id = get(n.attributes, L"ResourceId");
                    const auto offset = number(required(n.attributes, L"Offset")), size = number(required(n.attributes, L"Size"));
                    const auto it = index.find(name.generic_wstring());
                    if (it == index.end() && offset == 0) {
                        complete = false; notes.push_back(L"缺少外置 Bundle 成员（未读取外部文件或下载）：" + name.native()); return;
                    }
                    require(it != index.end(), Status::corrupt, L"Bundle 声明的内嵌成员缺失：" + name.native());
                    const auto i = it->second; const auto member = archive.zip_member(i);
                    require(member.method == codecs::Compression::stored, Status::unsupported, L"Bundle 成员不能再以 ZIP 压缩保存。");
                    require(offset == member.data_offset && size == member.packed.size() && size == archive.catalog().files[i].size, Status::corrupt, L"Bundle 成员偏移或大小与 ZIP 不符。");
                    ArchivePackage child(path, member.packed); std::vector<fs::path> child_paths; const auto child_index = index_files(child, child_paths);
                    require(!child_index.contains(std::wstring(bundle_manifest)), Status::unsupported, L"Bundle 成员不能是另一份 Bundle。");
                    const auto child_id = manifest(child.read_metadata(find(child_index, L"AppxManifest.xml"), xml_limit));
                    require(child_id.name == identity.name && child_id.publisher == identity.publisher && child_id.ver == ver && child_id.arch == arch &&
                            child_id.resource_id == resource_id && child_id.resource == (kind == L"resource"), Status::corrupt, L"Bundle 成员身份、架构或资源属性与清单不符：" + name.native());
                    require(msix_extension(name) && (platform::equal_name(name.extension().native(), L".appx") || platform::equal_name(name.extension().native(), L".msix")), Status::unsupported, L"Bundle 成员扩展名不受支持。");
                    paths[i] = fs::path(L"packages") / kind / arch / name; members.insert(i); current_member = i;
                    conditions[i] = L"Type=" + kind + L"; Architecture=" + arch + L"; ResourceId=" + resource_id + L"; Version=" + ver;
                    if (n.attributes.contains(L"IsStub") && boolean(get(n.attributes, L"IsStub"))) {
                        complete = false; notes.push_back(L"Bundle 成员是占位包，不代表完整应用：" + name.native());
                    }
                } else if (n.depth == 4 && current_member && parents[1] == L"Packages" && parents[2] == L"Package" &&
                           (parents[3] == L"Resources" || parents[3] == L"Dependencies")) {
                    conditions[*current_member] += L"; " + n.name + L"[" + n.ns + L"] ";
                    for (const auto& [key, value] : n.attributes) conditions[*current_member] += key + L"=" + value + L"; ";
                } else if (n.depth <= 2 && n.depth > 0) {
                    throw Failure(Status::unsupported, L"暂不支持此 Bundle 扩展（例如 Optional 包关系）。");
                }
            });
            require(root && identity_found && packages_found && !references.empty(), Status::corrupt, L"Bundle 缺少身份或包列表。");
            for (const auto& [name, i] : index) {
                if (members.contains(i) || map.files.contains(i) || platform::equal_name(name, L"[Content_Types].xml") || platform::equal_name(name, L"AppxBlockMap.xml") ||
                    platform::equal_name(name, L"AppxSignature.p7x") || platform::equal_name(name, L"AppxMetadata/CodeIntegrity.cat")) continue;
                throw Failure(Status::corrupt, L"Bundle 含未声明的载荷：" + name);
            }
            notes.push_back(L"Bundle：" + identity.name + L"; Publisher=" + identity.publisher + L"。所有内嵌架构和资源包分别展开到 packages；不按当前系统筛选，不合并资源。成员块哈希由递归单包校验。");
        }
        notes.push_back(L"静态 MSIX/APPX 提取，保留 VFS、清单和资源目录；校验块内容但不验证发布者签名，不部署、不注册、不运行应用或下载依赖。依赖安装身份的应用不保证可直接运行。");
        archive.set_layout(bundle ? L"MSIX/APPX Bundle" : L"MSIX/APPX", package_version, paths, std::move(notes), complete, conditions);
        std::vector<std::wstring> algorithms(paths.size());
        for (const auto& [idx, unused] : map.files) algorithms[idx] = L"SHA-" + std::to_wstring(map.digest_size * 8);
        archive.set_block_verifier([this](std::size_t idx, std::uint64_t offset, io::Bytes bytes) {
            const auto found = map.files.find(idx); if (found == map.files.end()) return;
            const auto& file = found->second;
            if (offset == 0 && !file.full_hash.empty()) full_digest = std::make_unique<Digest>(map.digest_size);
            if (bytes.empty()) {
                if (full_digest) { require(full_digest->finish() == file.full_hash, Status::corrupt, L"MSIX FileHash 校验失败。"); full_digest.reset(); }
                return;
            }
            require(offset % 65536 == 0 && offset / 65536 < file.hashes.size() && platform::hash_bytes(bytes, map.digest_size) == file.hashes[static_cast<std::size_t>(offset / 65536)],
                    Status::corrupt, L"MSIX 块哈希校验失败：" + archive.catalog().files[idx].path.native() + L"，块 " + std::to_wstring(offset / 65536 + 1));
            if (full_digest) full_digest->add(bytes);
        }, algorithms);
    }
};
MsixPackage::MsixPackage(const fs::path& path) {
    const auto ext = path.extension().native();
    require(!platform::equal_name(ext, L".emsix") && !platform::equal_name(ext, L".eappx") && !platform::equal_name(ext, L".emsixbundle") && !platform::equal_name(ext, L".eappxbundle"),
            Status::unsupported, L"暂不支持加密 MSIX/APPX 包，请提供未加密的离线包。");
    impl_ = std::make_unique<Impl>(path);
}
MsixPackage::~MsixPackage() = default;
const Catalog& MsixPackage::catalog() const { return impl_->archive.catalog(); }
fs::path MsixPackage::extract(const fs::path& parent) { return impl_->archive.extract(parent); }
}
