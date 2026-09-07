#include "formats/msi.h"
#include "platform/log.h"
#include "core/progress.h"
#include "codecs/cab.h"
#include "platform/files.h"
#include "io/temporary.h"

#include <msi.h>
#include <msiquery.h>
#include <oleauto.h>
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>

namespace extract::formats {
namespace {
struct MsiHandle {
    MSIHANDLE value = 0;
    ~MsiHandle() { if (value != 0) MsiCloseHandle(value); }
    MsiHandle() = default;
    MsiHandle(const MsiHandle&) = delete;
    MsiHandle& operator=(const MsiHandle&) = delete;
};
void check(UINT code, const wchar_t* operation) {
    if (code == ERROR_SUCCESS) return;
    const bool io = code == ERROR_ACCESS_DENIED || code == ERROR_DISK_FULL || code == ERROR_SHARING_VIOLATION;
    throw Failure(io ? Status::io_error : Status::corrupt,
                  std::wstring(operation) + L"（MSI " + std::to_wstring(code) + L"）", code);
}
std::wstring string_field(MSIHANDLE record, UINT column) {
    DWORD length = 0;
    wchar_t unused = 0;
    const auto result = MsiRecordGetStringW(record, column, &unused, &length);
    require(result == ERROR_MORE_DATA || result == ERROR_SUCCESS, Status::corrupt, L"无法读取 MSI 字符串。");
    require(length < 32768, Status::limit_exceeded, L"MSI 字符串超限。");
    std::vector<wchar_t> text(static_cast<std::size_t>(length) + 1);
    DWORD capacity = static_cast<DWORD>(text.size());
    check(MsiRecordGetStringW(record, column, text.data(), &capacity), L"读取 MSI 字符串失败");
    std::wstring value(text.data(), capacity);
    require(value.find(L'\0') == value.npos, Status::corrupt, L"MSI 字符串包含空字符。");
    (void)platform::utf8(value);
    return value;
}
int integer_field(MSIHANDLE record, UINT column) {
    require(!MsiRecordIsNull(record, column), Status::corrupt, L"MSI 必需整数为空。");
    return MsiRecordGetInteger(record, column);
}
void rows(MSIHANDLE database, const wchar_t* sql, const std::function<void(MSIHANDLE)>& visit) {
    MsiHandle view;
    check(MsiDatabaseOpenViewW(database, sql, &view.value), L"打开 MSI 查询失败");
    check(MsiViewExecute(view.value, 0), L"执行 MSI 查询失败");
    for (;;) {
        MsiHandle record;
        const auto result = MsiViewFetch(view.value, &record.value);
        if (result == ERROR_NO_MORE_ITEMS) break;
        check(result, L"读取 MSI 记录失败");
        visit(record.value);
    }
}
struct NameLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
void identifier(std::wstring_view text) {
    // 近期 Python MSI 的文件和组件键可超过常见的 72 字符列宽。
    require(!text.empty() && text.size() <= 255, Status::corrupt, L"MSI 标识长度无效。");
    for (wchar_t c : text)
        require((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'.',
                Status::corrupt, L"MSI 标识字符无效。");
}
std::wstring long_name(const std::wstring& text) {
    const auto separator = text.find(L'|');
    require(separator == text.npos || text.find(L'|', separator + 1) == text.npos, Status::corrupt, L"长短名称格式无效。");
    return separator == text.npos ? text : text.substr(separator + 1);
}
bool system_directory(const std::wstring& id) {
    for (const auto* name : {L"ProgramFilesFolder", L"ProgramFiles64Folder", L"ProgramFiles6432Folder",
        L"CommonFilesFolder", L"CommonFiles64Folder", L"CommonFiles6432Folder", L"WindowsFolder",
        L"SystemFolder", L"System64Folder", L"System16Folder", L"DesktopFolder", L"CommonAppDataFolder",
        L"LocalAppDataFolder", L"AppDataFolder", L"PersonalFolder", L"StartMenuFolder", L"ProgramMenuFolder",
        L"StartupFolder", L"FontsFolder", L"TempFolder", L"AdminToolsFolder", L"NetHoodFolder",
        L"PrintHoodFolder", L"RecentFolder", L"SendToFolder", L"FavoritesFolder", L"MyPicturesFolder", L"TemplateFolder"})
        if (platform::equal_name(id, name)) return true;
    return false;
}
}

struct MsiPackage::Impl {
    Catalog catalog;
    std::vector<platform::Handle> input_ancestors;
    platform::Handle input;
    MsiHandle database;
    struct Media { int disk, last; std::wstring cabinet; std::vector<std::size_t> files; };
    struct Source { fs::path path; bool compressed; };
    std::vector<Media> media;
    std::vector<Source> sources;
    std::size_t metadata_characters = 0;

    std::wstring text_field(MSIHANDLE record, UINT column) {
        auto value = string_field(record, column);
        constexpr std::size_t limit = 8 * 1024 * 1024;
        require(value.size() <= limit - metadata_characters, Status::limit_exceeded, L"MSI 文本元数据超限。");
        metadata_characters += value.size();
        return value;
    }

    explicit Impl(const fs::path& path) {
        catalog.input = platform::absolute_path(path);
        input_ancestors = platform::lock_ancestors(catalog.input.parent_path());
        input = platform::Handle(CreateFileW(platform::extended_path(catalog.input).c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!input) platform::io_failure(L"无法打开安装包");
        LARGE_INTEGER input_size{};
        if (!GetFileSizeEx(input.get(), &input_size)) platform::io_failure(L"无法读取安装包大小");
        require(input_size.QuadPart >= 0, Status::corrupt, L"MSI 文件大小无效。");
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(input.get(), FileAttributeTagInfo, &attributes, sizeof(attributes))) platform::io_failure(L"无法检查安装包");
        require((attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0,
                Status::unsafe_path, L"输入必须是普通文件，不能是链接或目录。");
        std::array<unsigned char, 8> signature{};
        DWORD read = 0;
        if (!ReadFile(input.get(), signature.data(), static_cast<DWORD>(signature.size()), &read, nullptr)) platform::io_failure(L"读取安装包标识失败");
        constexpr std::array<unsigned char, 8> compound = {0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1};
        require(read == signature.size() && signature == compound, Status::unsupported, L"输入不是 MSI 数据库。");
        const auto opened = MsiOpenDatabaseW(platform::extended_path(catalog.input).c_str(), MSIDBOPEN_READONLY, &database.value);
        if (opened == ERROR_OPEN_FAILED || opened == ERROR_INSTALL_PACKAGE_INVALID)
            throw Failure(Status::corrupt, L"复合文件不能作为 MSI 数据库读取，可能损坏或不是 MSI。", opened);
        check(opened, L"只读打开 MSI 失败");
        for (const auto* table : {L"File", L"Component", L"Directory", L"Media"})
            require(MsiDatabaseIsTablePersistentW(database.value, table) == MSICONDITION_TRUE,
                    Status::unsupported, L"数据库缺少标准 MSI 文件布局表。");
        read_catalog();
    }

    void read_catalog() {
        MsiHandle summary;
        check(MsiGetSummaryInformationW(database.value, nullptr, 0, &summary.value), L"读取 MSI 摘要失败");
        UINT type = 0;
        INT word_count = 0;
        DWORD text_length = 0;
        check(MsiSummaryInfoGetPropertyW(summary.value, 15, &type, &word_count, nullptr, nullptr, &text_length), L"读取压缩标志失败");
        require(type == VT_I4, Status::corrupt, L"MSI 摘要缺少源文件压缩标志。");
        require((word_count & 4) == 0, Status::unsupported, L"暂不支持管理安装映像。");
        const bool default_compressed = (word_count & 2) != 0;
        const bool short_source = (word_count & 1) != 0;
        const auto source_name = [&](const std::wstring& value) {
            (void)long_name(value);
            return short_source ? value.substr(0, value.find(L'|')) : long_name(value);
        };
        struct Directory { std::wstring parent, target, source; fs::path resolved, source_path; int state = 0; };
        std::map<std::wstring, Directory, NameLess> directories;
        rows(database.value, L"SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`", [&](MSIHANDLE r) {
            auto id = text_field(r, 1); identifier(id);
            auto parent = text_field(r, 2); if (!parent.empty()) identifier(parent);
            auto value = text_field(r, 3);
            auto colon = value.find(L':');
            require(colon == value.npos || value.find(L':', colon + 1) == value.npos, Status::corrupt, L"MSI 源/目标目录格式无效。");
            auto target = long_name(value.substr(0, colon));
            auto source = source_name(colon == value.npos ? value : value.substr(colon + 1));
            require(directories.emplace(id, Directory{parent, target, source, {}, {}, 0}).second, Status::corrupt, L"MSI 目录标识重复。");
        });
        std::function<fs::path(const std::wstring&, unsigned)> resolve = [&](const std::wstring& id, unsigned depth) -> fs::path {
            require(depth <= 64, Status::limit_exceeded, L"MSI 目录层级超限。");
            auto it = directories.find(id);
            require(it != directories.end(), Status::corrupt, L"MSI 引用了不存在的目录。");
            auto& row = it->second;
            require(row.state != 1, Status::corrupt, L"MSI 目录父链成环。");
            if (row.state == 2) return row.resolved;
            row.state = 1;
            if (platform::equal_name(id, L"TARGETDIR")) {
                require(row.parent.empty() || platform::equal_name(row.parent, id), Status::corrupt, L"TARGETDIR 根目录关系无效。");
            } else {
                require(!row.parent.empty() && !platform::equal_name(row.parent, id), Status::corrupt, L"MSI 存在额外根目录。");
                const auto parent = resolve(row.parent, depth + 1);
                row.source_path = directories.at(row.parent).source_path;
                if (row.source != L".") { platform::validate_component(row.source); row.source_path /= row.source; }
                if (system_directory(id)) row.resolved = id;
                else {
                    row.resolved = parent;
                    if (row.target != L".") { platform::validate_component(row.target); row.resolved /= row.target; }
                }
            }
            row.state = 2;
            return row.resolved;
        };
        for (const auto& [id, row] : directories) { (void)row; (void)resolve(id, 0); }

        std::map<std::wstring, std::wstring, NameLess> components;
        rows(database.value, L"SELECT `Component`, `Directory_` FROM `Component`", [&](MSIHANDLE r) {
            auto id = text_field(r, 1); identifier(id);
            auto directory = text_field(r, 2); (void)resolve(directory, 0);
            require(components.emplace(id, directory).second, Status::corrupt, L"MSI 组件标识重复。");
        });
        int last_sequence = 0;
        int last_disk = 0;
        std::set<std::wstring, NameLess> cabinet_names;
        rows(database.value, L"SELECT `DiskId`, `LastSequence`, `Cabinet` FROM `Media` ORDER BY `DiskId`", [&](MSIHANDLE r) {
            const auto disk = integer_field(r, 1), last = integer_field(r, 2);
            require(disk > last_disk && last >= last_sequence, Status::corrupt, L"MSI 媒体顺序无效。");
            last_disk = disk; last_sequence = last;
            auto cabinet = text_field(r, 3);
            if (!cabinet.empty()) {
                require(cabinet_names.insert(cabinet).second, Status::corrupt, L"MSI 媒体重复引用 CAB。");
                if (cabinet[0] == L'#') require(cabinet.size() > 1, Status::corrupt, L"MSI 内嵌流名称为空。");
                else platform::validate_component(cabinet);
            }
            media.push_back({disk, last, cabinet, {}});
        });
        if (media.size() == 1 && !media[0].cabinet.empty())
            catalog.cabinet = media[0].cabinet[0] == L'#' ? media[0].cabinet.substr(1) : media[0].cabinet;
        for (const auto& item : media) catalog.notes.push_back(L"Media " + std::to_wstring(item.disk) + L"：" +
            (item.cabinet.empty() ? L"松散源文件" : item.cabinet));
        std::set<int> sequences;
        std::set<std::wstring, NameLess> identifiers;
        rows(database.value, L"SELECT `File`, `Component_`, `FileName`, `FileSize`, `Attributes`, `Sequence` FROM `File` ORDER BY `Sequence`", [&](MSIHANDLE r) {
            Entry entry;
            entry.id = text_field(r, 1); identifier(entry.id);
            require(identifiers.insert(entry.id).second, Status::corrupt, L"MSI 文件标识重复。");
            const auto component = components.find(text_field(r, 2));
            require(component != components.end(), Status::corrupt, L"MSI 文件引用了不存在的组件。");
            const auto names = text_field(r, 3);
            auto filename = long_name(names);
            platform::validate_component(filename);
            const auto& directory = directories.at(component->second);
            entry.path = entry.original_path = directory.resolved / filename;
            platform::validate_relative(entry.path);
            const int size = integer_field(r, 4);
            const int flags = MsiRecordIsNull(r, 5) ? 0 : integer_field(r, 5);
            const int sequence = integer_field(r, 6);
            require(size >= 0 && sequence > 0 && sequence <= last_sequence,
                    Status::corrupt, L"MSI 文件大小或媒体顺序无效。");
            require((flags & 0x6000) != 0x6000, Status::corrupt, L"MSI 压缩标志互相冲突。");
            const bool compressed = (flags & 0x4000) != 0 || ((flags & 0x2000) == 0 && default_compressed);
            require(!compressed || sequences.insert(sequence).second, Status::corrupt, L"MSI 压缩文件序号重复。");
            auto disk = std::find_if(media.begin(), media.end(), [&](const Media& item) { return sequence <= item.last; });
            require(disk != media.end(), Status::corrupt, L"MSI 文件没有对应媒体。");
            fs::path source;
            if (compressed) {
                require(!disk->cabinet.empty(), Status::corrupt, L"压缩文件没有对应 CAB。");
                disk->files.push_back(catalog.files.size());
                entry.source_expression = disk->cabinet + L":" + entry.id;
            } else {
                const auto name = source_name(names); platform::validate_component(name);
                source = directory.source_path / name; platform::validate_relative(source);
                entry.source_expression = source.generic_wstring();
            }
            sources.push_back({std::move(source), compressed});
            entry.size = static_cast<std::uint64_t>(size);
            require(entry.size <= max_file_bytes - catalog.total_size, Status::limit_exceeded, L"预计展开大小超出 64 位文件范围。");
            catalog.total_size += entry.size;
            catalog.files.push_back(std::move(entry));
        });
        if (catalog.files.empty()) catalog.notes.push_back(L"此 MSI 的 File 表为空，没有应用文件载荷；仅生成清单，不运行配置动作或导出 Binary 辅助流。");
        if (MsiDatabaseIsTablePersistentW(database.value, L"MsiFileHash") == MSICONDITION_TRUE) {
            rows(database.value, L"SELECT `File_`, `Options`, `HashPart1`, `HashPart2`, `HashPart3`, `HashPart4` FROM `MsiFileHash`", [&](MSIHANDLE r) {
                const auto key = text_field(r, 1);
                auto file = std::find_if(catalog.files.begin(), catalog.files.end(), [&](const Entry& e) { return platform::equal_name(key, e.id); });
                require(file != catalog.files.end() && !file->msi_hash, Status::corrupt, L"MSI 文件哈希引用无效。");
                require(integer_field(r, 2) == 0, Status::unsupported, L"不支持此 MSI 文件哈希选项。");
                std::array<DWORD, 4> hash{};
                for (UINT i = 0; i < 4; ++i) hash[i] = static_cast<DWORD>(integer_field(r, i + 3));
                file->msi_hash = hash;
            });
        }
        plan_paths(catalog);
    }

    std::unique_ptr<io::TemporaryFile> cabinet_bytes(const std::wstring& name) {
        MsiHandle view, parameter, record;
        check(MsiDatabaseOpenViewW(database.value, L"SELECT `Data` FROM `_Streams` WHERE `Name` = ?", &view.value), L"打开内嵌流查询失败");
        parameter.value = MsiCreateRecord(1);
        require(parameter.value != 0, Status::internal_error, L"无法分配 MSI 查询参数。");
        check(MsiRecordSetStringW(parameter.value, 1, name.c_str()), L"设置 CAB 流名称失败");
        check(MsiViewExecute(view.value, parameter.value), L"查询 CAB 流失败");
        check(MsiViewFetch(view.value, &record.value), L"内嵌 CAB 流不存在");
        auto result = std::make_unique<io::TemporaryFile>(L"msi-cab-cache");
        std::array<std::byte, 65536> chunk{};
        for (;;) {
            DWORD count = static_cast<DWORD>(chunk.size());
            check(MsiRecordReadStream(record.value, 1, reinterpret_cast<char*>(chunk.data()), &count), L"读取 CAB 数据失败");
            if (count == 0) break;
            result->append(std::span(chunk).first(count));
            progress::advance(count);
        }
        return result;
    }
};

MsiPackage::MsiPackage(const fs::path& input) : impl_(std::make_unique<Impl>(input)) {}
MsiPackage::~MsiPackage() = default;
const Catalog& MsiPackage::catalog() const { return impl_->catalog; }
fs::path MsiPackage::extract(const fs::path& parent) {
    io::Output output(parent.empty() ? impl_->catalog.input.parent_path() : parent, impl_->catalog.input.stem().wstring());
    for (const auto& media : impl_->media) {
        if (media.cabinet.empty()) continue;
        log::Scope media_step(L"msi.cabinet");
        log::write(log::Level::info, L"msi.media", media.cabinet);
        std::unique_ptr<io::TemporaryFile> embedded;
        std::unique_ptr<io::Input> external;
        io::Bytes bytes;
        if (media.cabinet[0] == L'#') {
            progress::Scope stage(progress::Phase::preparing, {}, media.cabinet);
            embedded = impl_->cabinet_bytes(media.cabinet.substr(1)); bytes = embedded->bytes();
        }
        else {
            try { external = std::make_unique<io::Input>(impl_->catalog.input.parent_path() / media.cabinet); }
            catch (const Failure& e) { throw Failure(e.status, L"读取 MSI 外置 CAB 失败：" + media.cabinet + L"；" + e.message, e.native_code); }
            bytes = external->bytes();
        }
        const auto members = codecs::cab_members(bytes);
        require(members.size() == media.files.size(), Status::corrupt, L"CAB 文件数与对应 MSI 媒体不符。");
        std::vector<Entry*> targets;
        std::set<std::size_t> seen;
        for (const auto& member : members) {
            identifier(member.decoded_name);
            const auto found = std::find_if(media.files.begin(), media.files.end(), [&](std::size_t i) {
                return platform::equal_name(impl_->catalog.files[i].id, member.decoded_name);
            });
            require(found != media.files.end() && seen.insert(*found).second, Status::corrupt, L"CAB 成员不属于此媒体或重复。");
            targets.push_back(&impl_->catalog.files[*found]);
        }
        codecs::extract_cab(bytes, targets, output);
    }
    for (std::size_t i = 0; i < impl_->sources.size(); ++i) {
        const auto& source = impl_->sources[i]; if (source.compressed) continue;
        auto& entry = impl_->catalog.files[i];
        progress::file(entry.path.native());
        log::Scope step(L"msi.loose_file", nullptr, &source.path);
        try {
            io::Input input(impl_->catalog.input.parent_path() / source.path);
            require(input.size() == entry.size, Status::corrupt, L"松散源文件大小与 File 表不符。");
            { auto file = output.create_file(entry.path); input.copy_to(file.get());
              if (!FlushFileBuffers(file.get())) platform::io_failure(L"保存松散源文件失败"); }
            codecs::verify_file(entry, output.full_path(entry.path));
        } catch (const Failure& e) { throw Failure(e.status, L"读取 MSI 松散源文件失败：" + source.path.wstring() + L"；" + e.message, e.native_code); }
    }
    progress::Scope finalizing(progress::Phase::finalizing);
    const auto report = platform::utf8(catalog_json(impl_->catalog, true));
    {
        auto file = output.create_file(L"_extract-report.json");
        platform::write_all(file.get(), std::as_bytes(std::span(report)));
        if (!FlushFileBuffers(file.get())) platform::io_failure(L"保存解包清单失败");
    }
    return output.commit();
}
} // namespace extract::formats
