#include "core/tree.h"
#include "formats/nsis.h"
#include "formats/pe.h"
#include "formats/archive.h"
#include "formats/cab.h"
#include "formats/burn.h"
#include "io/input.h"
#include "platform/log.h"
#include <algorithm>
#include <map>
#include <set>

namespace extract {
namespace {
enum class Kind { none, msi, inno, nsis, zip, seven_zip, cab, burn, uninstaller };

template<std::size_t N>
bool starts(io::Bytes bytes, const std::array<unsigned char, N>& signature) {
    if (bytes.size() < N) return false;
    for (std::size_t i = 0; i < N; ++i) if (std::to_integer<unsigned char>(bytes[i]) != signature[i]) return false;
    return true;
}
Kind probe(io::Bytes bytes, bool archives) {
    if (starts(bytes, std::array<unsigned char, 8>{0xd0,0xcf,0x11,0xe0,0xa1,0xb1,0x1a,0xe1})) return Kind::msi;
    if (archives && starts(bytes, std::array<unsigned char, 4>{'M','S','C','F'})) return Kind::cab;
    if (archives && starts(bytes, std::array<unsigned char, 6>{'7','z',0xbc,0xaf,0x27,0x1c})) return Kind::seven_zip;
    if (archives && (starts(bytes, std::array<unsigned char, 4>{'P','K',3,4}) ||
        starts(bytes, std::array<unsigned char, 4>{'P','K',5,6}))) return Kind::zip;
    if (!starts(bytes, std::array<unsigned char, 2>{'M','Z'})) return Kind::none;
    try {
        if (formats::burn_probe(bytes)) return Kind::burn;
        if (formats::nsis_probe(bytes)) return formats::nsis_uninstaller(bytes) ? Kind::uninstaller : Kind::nsis;
        if (const auto archive = formats::archive_probe(bytes))
            return archive->kind == formats::ArchiveKind::zip ? Kind::zip : Kind::seven_zip;
        if (formats::cab_probe(bytes)) return Kind::cab;
        const auto resource = formats::pe_rcdata(bytes, 11111);
        if (starts(resource, std::array<unsigned char, 12>{'r','D','l','P','t','S',0xcd,0xe6,0xd7,0x7b,0x0b,0x2a}))
            return Kind::inno;
    } catch (const Failure& failure) {
        // 普通应用 EXE 不是内层包。只有实际安装器标记才触发自动展开。
        if (failure.status != Status::corrupt && failure.status != Status::unsupported && failure.status != Status::limit_exceeded) throw;
    }
    return Kind::none;
}
std::wstring name(Kind kind) {
    switch (kind) {
    case Kind::msi: return L"MSI";
    case Kind::cab: return L"CAB";
    case Kind::burn: return L"WiX Burn";
    case Kind::inno: return L"Inno Setup";
    case Kind::nsis: return L"NSIS";
    case Kind::zip: return L"ZIP";
    case Kind::seven_zip: return L"7z";
    case Kind::uninstaller: return L"NSIS uninstaller";
    default: return L"";
    }
}
void save_report(const fs::path& directory, const Catalog& catalog) {
    log::Scope step(L"report.save", nullptr, &directory);
    const auto locks = platform::lock_ancestors(directory);
    const auto temporary = directory / (L".report-" + platform::unique_id() + L".tmp");
    platform::Handle file(CreateFileW(platform::extended_path(temporary).c_str(), GENERIC_WRITE | DELETE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file) platform::io_failure(L"无法创建递归解包报告");
    try {
        const auto bytes = platform::utf8(catalog_json(catalog, true));
        platform::write_all(file.get(), std::as_bytes(std::span(bytes)));
        if (!FlushFileBuffers(file.get())) platform::io_failure(L"无法保存递归解包报告");
        DWORD result = ERROR_SUCCESS;
        for (unsigned attempt = 0; attempt < 21; ++attempt) {
            result = platform::rename_in_place(file.get(), L"_extract-report.json", true);
            if (result != ERROR_ACCESS_DENIED && result != ERROR_SHARING_VIOLATION) break;
            if (attempt != 20) Sleep(100); // 大批量写入时，索引器可能短暂占用上一份报告。
        }
        if (result != ERROR_SUCCESS) platform::io_failure(L"无法提交递归解包报告", result);
    } catch (...) {
        file.reset(); DeleteFileW(platform::extended_path(temporary).c_str()); throw;
    }
}

struct Context {
    std::uint64_t reserved_bytes = 0;
    std::size_t reserved_files = 0;
    unsigned package_count = 0;
    std::set<std::wstring> active;
    std::map<std::wstring, ExtractionResult> completed;

    ExtractionResult process(std::unique_ptr<Package> package, const fs::path& parent, unsigned depth,
                             const std::wstring& hash) {
        const auto& planned = package->catalog();
        const auto package_path = planned.input;
        log::Scope step(L"package.extract", &package_path);
        log::detail(log::Level::info, L"extraction.begin", [&] {
            return L"format=" + planned.format + L"; depth=" + std::to_wstring(depth) + L"; sha256=" + hash +
                L"; files=" + std::to_wstring(planned.files.size()) + L"; bytes=" + std::to_wstring(planned.total_size);
        });
        reserved_bytes = checked_size_sum(reserved_bytes, planned.total_size);
        require(planned.files.size() <= (std::numeric_limits<std::size_t>::max)() - reserved_files,
            Status::limit_exceeded, L"文件统计超过当前程序架构的表示范围。");
        reserved_files += planned.files.size();
        platform::ensure_disk_space(parent.empty() ? planned.input.parent_path() : parent, planned.total_size);
        ++package_count;
        active.insert(hash);
        struct ActiveGuard {
            std::set<std::wstring>& values;
            const std::wstring& hash;
            ~ActiveGuard() { values.erase(hash); }
        } active_guard{active, hash};
        ExtractionResult result;
        result.output = package->extract(parent);
        Catalog catalog = package->catalog();
        package.reset(); // 递归前释放上层映射与 Solid 磁盘缓存。
        result.primary_output = result.output;
        result.complete = catalog.required_paths_resolved() && catalog.content_complete;
        if (!catalog.content_complete) for (const auto& note : catalog.notes) result.details += note + L"\r\n";
        result.total_bytes = catalog.total_size; result.file_count = catalog.files.size();
        const auto rooted_at_app = [](const Entry& entry) {
            return !entry.path.empty() && platform::equal_name(entry.path.begin()->wstring(), L"app");
        };
        catalog.nested_complete = false; // 尚未检查内层时不能留下一个最终完成的报告。
        catalog.tree_total_bytes = result.total_bytes; catalog.tree_file_count = result.file_count;
        save_report(result.output, catalog);
        bool nested_ok = true;
        std::vector<fs::path> inner_primary;
        std::set<fs::path> archive_primary;
        for (auto& entry : catalog.files) {
            if (!entry.path_resolved) log::detail(entry.is_uninstaller ? log::Level::info : log::Level::warning, L"path.unresolved", [&] {
                return entry.path.wstring() + L"; source=" + entry.source_expression + L"; auxiliary=" + (entry.is_uninstaller ? L"true" : L"false");
            });
            const auto extension = entry.path.extension().wstring();
            const bool executable = platform::equal_name(extension, L".exe") || platform::equal_name(extension, L".msi");
            const auto root = entry.path.begin()->wstring();
            const bool archive = ((catalog.format == L"NSIS" && !platform::equal_name(root, L"app")) || catalog.format == L"CAB") &&
                (platform::equal_name(extension, L".zip") || platform::equal_name(extension, L".7z") || platform::equal_name(extension, L".cab"));
            if (!executable && !archive) continue;
            const auto path = result.output / entry.path;
            log::Scope child_step(L"nested.inspect", &path);
            Catalog::Nested nested; nested.input = entry.path;
            try {
                Kind kind;
                {
                    io::Input input(path);
                    std::array<std::byte, 8> header{};
                    input.read(0, std::span(header).first(static_cast<std::size_t>((std::min)(input.size(), std::uint64_t{8}))));
                    kind = starts(std::span<const std::byte>(header), std::array<unsigned char, 8>{0xd0,0xcf,0x11,0xe0,0xa1,0xb1,0x1a,0xe1})
                        ? Kind::msi : probe(input.bytes(), archive);
                    if (kind == Kind::uninstaller)
                        require(platform::sha256(path) == entry.sha256, Status::corrupt, L"卸载辅助文件在提取后被修改。");
                }
                if (kind == Kind::none) { log::write(log::Level::info, L"nested.skipped", L"未发现安装器标识，保留普通文件"); continue; }
                nested.format = name(kind);
                log::write(log::Level::info, L"nested.detected", nested.format);
                if (kind == Kind::uninstaller) {
                    entry.is_uninstaller = true;
                    nested.status = L"preserved";
                    nested.message = L"卸载器作为辅助文件保留，不递归展开；其原始目录是否还原不影响解包完成状态。";
                    log::write(log::Level::info, L"nested.preserved", nested.message);
                    if (!entry.path_resolved) {
                        const auto note = L"卸载辅助文件已保留，原始目录未还原不影响解包完成：" + entry.path.generic_wstring();
                        catalog.notes.push_back(note); result.details += note + L"\r\n";
                    }
                    catalog.nested_packages.push_back(std::move(nested));
                    continue;
                }
                require(depth < 3 && package_count < 32, Status::limit_exceeded, L"递归展开达到 4 层或 32 个安装包的上限。");
                require(!active.contains(entry.sha256), Status::limit_exceeded, L"检测到重复嵌套链，已停止展开。");
                // 打开后输入句柄锁定文件，再比对上层提取时记录的摘要，防止阶段间替换。
                auto child = open_package(path);
                require(platform::sha256(path) == entry.sha256, Status::corrupt, L"内嵌安装包在提取后被修改。");
                ExtractionResult child_result;
                if (const auto cached = completed.find(entry.sha256); cached != completed.end() && kind != Kind::msi && kind != Kind::burn) {
                    child_result = cached->second;
                    nested.message = L"相同内容已展开，复用已有结果。";
                    log::detail(log::Level::info, L"nested.reused", [&] { return child_result.output.wstring(); });
                } else {
                    child_result = process(std::move(child), result.output, depth + 1, entry.sha256);
                    result.total_bytes += child_result.total_bytes; result.file_count += child_result.file_count;
                }
                nested.output = child_result.output.lexically_relative(result.output);
                nested.status = child_result.complete ? L"complete" : L"partial";
                nested_ok = nested_ok && child_result.complete;
                result.details += L"内层 " + nested.format + L"：" + path.wstring() + L"\r\n输出：" + child_result.primary_output.wstring() + L"\r\n";
                result.details += child_result.details;
                if (child_result.complete && std::find(inner_primary.begin(), inner_primary.end(), child_result.primary_output) == inner_primary.end())
                    inner_primary.push_back(child_result.primary_output);
                if (child_result.complete && (kind == Kind::zip || kind == Kind::seven_zip))
                    archive_primary.insert(child_result.primary_output);
            } catch (const Failure& failure) {
                log::failure(L"nested.failed", failure);
                nested_ok = false;
                nested.status = failure.status == Status::unsupported ? L"unsupported" :
                    (failure.status == Status::limit_exceeded ? L"limit" : L"failed");
                nested.message = failure.message;
                result.details += L"内层未完成：" + path.wstring() + L"\r\n原因：" + failure.message + L"\r\n";
            }
            catalog.nested_packages.push_back(std::move(nested));
        }
        result.complete = catalog.required_paths_resolved() && catalog.content_complete && nested_ok;
        catalog.nested_scanned = true; catalog.nested_complete = nested_ok;
        catalog.tree_total_bytes = result.total_bytes; catalog.tree_file_count = result.file_count;
        const bool app_files = std::any_of(catalog.files.begin(), catalog.files.end(), [&](const Entry& entry) {
            return !entry.is_uninstaller && rooted_at_app(entry);
        });
        const bool app_executable = std::any_of(catalog.files.begin(), catalog.files.end(), [&](const Entry& entry) {
            return !entry.is_uninstaller && rooted_at_app(entry) && platform::equal_name(entry.path.extension().wstring(), L".exe");
        });
        if (app_files) result.primary_output /= L"app";
        if ((catalog.format == L"NSIS" || catalog.format == L"WiX Burn" || catalog.format == L"CAB") && inner_primary.size() == 1 && result.complete) {
            // NSIS 外层可能只把卸载图标等资源放入 app，真正的应用在内嵌归档中。
            if (!app_files || (catalog.format == L"NSIS" && !app_executable && archive_primary.contains(inner_primary.front())))
                result.primary_output = inner_primary.front();
        }
        save_report(result.output, catalog);
        log::detail(result.complete ? log::Level::info : log::Level::warning, L"extraction.end", [&] {
            return std::wstring(L"status=") + (result.complete ? L"complete" : L"partial") + L"; output=" + result.primary_output.wstring() +
                L"; treeFiles=" + std::to_wstring(result.file_count) + L"; treeBytes=" + std::to_wstring(result.total_bytes) +
                L"; requiredPathsResolved=" + (catalog.required_paths_resolved() ? L"true" : L"false") +
                L"; contentComplete=" + (catalog.content_complete ? L"true" : L"false") + L"; nestedComplete=" + (nested_ok ? L"true" : L"false");
        });
        completed[hash] = result;
        return result;
    }
};
}

ExtractionResult extract_tree(std::unique_ptr<Package> package, const fs::path& parent) {
    Context context;
    const auto hash = platform::sha256(package->catalog().input);
    return context.process(std::move(package), parent, 0, hash);
}
} // namespace extract
