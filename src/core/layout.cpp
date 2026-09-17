#include "core/layout.h"
#include "core/control.h"
#include "core/progress.h"
#include "io/input.h"
#include "io/output.h"
#include <algorithm>
#include <map>
#include <set>

namespace extract {
namespace {
struct NameLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
bool same(const fs::path& a, const fs::path& b) { return platform::equal_name(a.generic_wstring(), b.generic_wstring()); }
std::optional<fs::path> below(const fs::path& path, const fs::path& prefix) {
    auto part = path.begin();
    for (const auto& item : prefix) {
        if (part == path.end() || !same(*part, item)) return {};
        ++part;
    }
    fs::path result;
    for (; part != path.end(); ++part) result /= *part;
    return result;
}
fs::path common_parent(fs::path a, const fs::path& b) {
    while (!a.empty() && !below(b, a)) a = a.parent_path();
    return a;
}
bool archive_layout(const Catalog& catalog) {
    return catalog.format == L"ZIP" || catalog.format == L"7z" || catalog.format.starts_with(L"MSIX") || catalog.format.starts_with(L"APPX");
}
bool reserved(const fs::path& path) {
    if (path.empty()) return false;
    const auto root = path.begin()->wstring();
    for (const auto name : {L"_extra", L"_packages", L"_extract-packages", L"_extract-script", L"_extract-report.json", L"_variants", L"_unresolved"})
        if (platform::equal_name(root, name)) return true;
    return false;
}
bool nested_file(const Catalog& catalog, const Entry& file) {
    return std::any_of(catalog.nested_packages.begin(), catalog.nested_packages.end(), [&](const auto& item) {
        return item.status != L"preserved" && same(item.input, file.path);
    });
}
struct Main { fs::path prefix, name; };
std::vector<Main> main_directories(const Catalog& catalog, bool archive_application) {
    const auto eligible = [&](const Entry& file) { return file.path_resolved && !file.is_uninstaller && !nested_file(catalog, file); };
    if (archive_layout(catalog)) return archive_application && std::any_of(catalog.files.begin(), catalog.files.end(), eligible)
        ? std::vector<Main>{{{}, {}}} : std::vector<Main>{};
    std::map<std::wstring, fs::path, NameLess> launches;
    for (const auto& action : catalog.runtime_actions) {
        if (action.kind != L"launch" && action.kind != L"launch-shell") continue;
        // A quoted command can retain its symbolic directory even when it is
        // not a bare resolved path. Match only a known file's exact expression.
        std::wstring quoted;
        if (action.kind == L"launch" && action.target.starts_with(L'"')) {
            const auto end = action.target.find(L'"', 1);
            if (end != action.target.npos && (end + 1 == action.target.size() || action.target[end + 1] == L' '))
                quoted = action.target.substr(1, end - 1);
        }
        for (const auto& file : catalog.files) if (eligible(file) && same(file.path, file.original_path)
            && platform::equal_name(file.path.extension().wstring(), L".exe") &&
            ((action.target_resolved && same(fs::path(action.target), file.original_path)) ||
                (!quoted.empty() && same(fs::path(quoted), fs::path(file.source_expression))))) {
            const auto directory = file.path.parent_path();
            if (!directory.empty()) launches.emplace(directory.generic_wstring(), directory);
        }
    }
    if (launches.size() > 1) return {}; // Different explicit launch locations: no preferred application.
    if (launches.size() == 1) return {{launches.begin()->second, {}}};
    std::map<std::wstring, fs::path, NameLess> roots;
    for (const auto& file : catalog.files) if (eligible(file)) {
        const auto root = file.path.begin()->wstring();
        if (platform::equal_name(root, L"app") || platform::equal_name(root, L"app-x86") ||
            platform::equal_name(root, L"app-x64") || platform::equal_name(root, L"app-arm64")) roots.emplace(root, fs::path(root));
    }
    // NSIS wrappers can leave only an uninstall icon/configuration in app;
    // those resources must not compete with the application in a nested package.
    if (catalog.format == L"NSIS" && std::any_of(catalog.nested_packages.begin(), catalog.nested_packages.end(),
        [](const auto& nested) { return nested.status == L"complete"; })) {
        std::erase_if(roots, [&](const auto& root) {
            return !std::any_of(catalog.files.begin(), catalog.files.end(), [&](const auto& file) {
                return eligible(file) && below(file.path, root.second) && platform::equal_name(file.path.extension().wstring(), L".exe");
            });
        });
    }
    std::vector<Main> result;
    for (const auto& [name, path] : roots) result.push_back({path, platform::equal_name(name, L"app") ? fs::path{} : fs::path(name.substr(4))});
    return result;
}
std::wstring group_name(const fs::path& path) {
    const auto value = path.generic_wstring();
    const std::pair<const wchar_t*, const wchar_t*> names[] = {
        {L"Public/Documents", L"PublicDocuments"}, {L"User/Documents", L"Documents"},
        {L"AppData/Roaming", L"Roaming"}, {L"AppData/Local", L"LocalAppData"},
        {L"Public/Desktop", L"PublicDesktop"}, {L"User/Desktop", L"Desktop"},
        {L"ProgramData", L"ProgramData"}, {L"plugins", L"Plugins"}, {L"temp", L"Temp"}, {L"app", L"App"}};
    for (const auto& [prefix, name] : names) if (below(path, prefix)) return name;
    // Keep distinct original roots separate, including unresolved/variant namespaces.
    return path.has_parent_path() ? path.begin()->wstring() : L"Other";
}
bool conflict(const std::vector<fs::path>& paths) {
    std::set<std::wstring, NameLess> names;
    for (const auto& path : paths) if (!names.insert(path.generic_wstring()).second) return true;
    for (const auto& path : paths) for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path())
        if (names.contains(parent.generic_wstring())) return true;
    return false;
}
}

ExtractionResult compact_output(std::vector<LayoutLayer>& layers, const fs::path& input, const fs::path& parent, bool complete) {
    require(!layers.empty() && !layers.front().directory.empty(), Status::internal_error, L"缺少目录整理的根包。");
    const auto count = layers.size();
    std::vector<fs::path> bases(count), reports(count), input_paths(count);
    std::vector<std::vector<Main>> mains(count);
    std::vector<std::vector<fs::path>> targets(count), originals(count);
    std::vector<bool> archive_application(count, false);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& layer = layers[i];
        const auto label = L"p" + std::to_wstring(i);
        const auto stem = layer.catalog.input.stem().wstring();
        bases[i] = fs::path(L"_packages") / (stem.size() <= 120 ? label + L"-" + stem : label);
        reports[i] = i ? fs::path(L"_extract-packages") / label / L"report.json" : fs::path(L"_extract-report.json");
        for (const auto& file : layer.catalog.files) originals[i].push_back(file.path);
        targets[i] = originals[i];
        if (layer.directory.empty()) continue;
        for (const auto& nested : layer.catalog.nested_packages) if (nested.status == L"complete") {
            const auto child = (layer.directory / nested.output).lexically_normal();
            for (std::size_t j = 0; j < count; ++j) if (same(layers[j].directory, child) &&
                (layers[j].catalog.format == L"ZIP" || layers[j].catalog.format == L"7z") &&
                (layer.catalog.format == L"NSIS" || layer.catalog.format == L"CAB")) archive_application[j] = true;
        }
    }
    std::size_t applications = 0;
    for (std::size_t i = 0; i < count; ++i) if (!layers[i].directory.empty() && complete) {
        mains[i] = main_directories(layers[i].catalog, archive_application[i]);
        if (!mains[i].empty()) ++applications;
    }
    auto plan = [&](bool simplify) {
        for (std::size_t i = 0; i < count; ++i) {
            const auto& catalog = layers[i].catalog;
            const auto base = applications > 1 || i ? bases[i] : fs::path{};
            struct Group { fs::path common; std::vector<std::size_t> files; };
            std::map<std::wstring, Group, NameLess> groups;
            for (std::size_t n = 0; n < catalog.files.size(); ++n) {
                const auto& file = catalog.files[n];
                targets[i][n] = (i ? bases[i] : fs::path{}) / file.path;
                if (!simplify || !applications || (archive_layout(catalog) && mains[i].empty())) continue;
                bool main = false;
                for (const auto& app : mains[i]) if (const auto relative = below(file.path, app.prefix)) {
                    targets[i][n] = (applications == 1 ? fs::path{} : base) / app.name / *relative;
                    main = true; break;
                }
                if (main) continue;
                auto& group = groups[group_name(file.path)];
                group.common = group.files.empty() ? file.path.parent_path() : common_parent(group.common, file.path.parent_path());
                group.files.push_back(n);
            }
            for (const auto& [name, group] : groups) for (const auto n : group.files) {
                const auto& file = catalog.files[n];
                // Unknown/variant paths keep their full internal structure.
                const auto relative = reserved(file.path) ? file.path : *below(file.path, group.common);
                targets[i][n] = fs::path(L"_extra") / (i ? L"p" + std::to_wstring(i) : L"") / name / relative;
            }
        }
    };
    plan(true);
    // A program resource with a reserved name must not mix with generated data.
    bool unsafe = false;
    std::vector<fs::path> planned;
    for (std::size_t i = 0; i < count; ++i) for (std::size_t n = 0; n < targets[i].size(); ++n) {
        planned.push_back(targets[i][n]);
        if (applications == 1) for (const auto& app : mains[i]) if (below(originals[i][n], app.prefix) && reserved(targets[i][n])) unsafe = true;
    }
    const bool fallback = unsafe || conflict(planned);
    if (fallback) plan(false);

    Catalog merged = layers.front().catalog;
    merged.layout = L"compact"; merged.requested_layout = L"compact"; merged.path_base = L"extraction-root";
    merged.files.clear(); merged.nested_packages.clear(); merged.total_size = 0;
    merged.paths_resolved = true; merged.content_complete = true; merged.nested_complete = complete;
    struct Source { fs::path path; std::wstring hash; std::uint64_t size; };
    std::vector<Source> sources;
    std::vector<std::vector<std::size_t>> entries(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto& catalog = layers[i].catalog;
        if (layers[i].directory.empty()) continue;
        merged.paths_resolved = merged.paths_resolved && catalog.paths_resolved;
        merged.content_complete = merged.content_complete && catalog.content_complete;
        for (std::size_t n = 0; n < catalog.files.size(); ++n) {
            auto file = catalog.files[n];
            sources.push_back({layers[i].directory / file.path, file.sha256, file.size});
            file.path = targets[i][n]; file.package_id = L"p" + std::to_wstring(i); file.id = file.package_id + L"-" + file.id;
            merged.total_size = checked_size_sum(merged.total_size, file.size);
            entries[i].push_back(merged.files.size()); merged.files.push_back(std::move(file));
        }
    }
    // Conflicting fallback paths are preserved as variants, never overwritten.
    plan_paths(merged);
    for (std::size_t i = 0; i < count; ++i) for (std::size_t n = 0; n < entries[i].size(); ++n) {
        layers[i].catalog.files[n].path = merged.files[entries[i][n]].path;
        layers[i].catalog.files[n].package_id = L"p" + std::to_wstring(i);
    }
    struct Auxiliary { Source source; fs::path target; };
    std::vector<Auxiliary> auxiliary;
    auto required = merged.total_size;
    for (std::size_t i = 0; i < count; ++i) {
        auto& catalog = layers[i].catalog;
        catalog.layout = L"compact"; catalog.requested_layout = L"compact"; catalog.path_base = L"extraction-root";
        if (!catalog.compiled_script || layers[i].directory.empty()) continue;
        auto& script = *catalog.compiled_script;
        const auto base = i ? reports[i].parent_path() : fs::path{};
        for (auto* path : {&script.text_path, &script.metadata_path}) {
            const bool text = path == &script.text_path;
            const auto size = text ? script.text_size : script.metadata_size;
            auxiliary.push_back({{layers[i].directory / *path, text ? script.text_sha256 : script.metadata_sha256, size}, base / *path});
            *path = base / *path; required = checked_size_sum(required, size);
        }
    }
    merged.compiled_script = layers.front().catalog.compiled_script;
    // Translate each edge using its source layer, including reused child results.
    for (std::size_t i = 0; i < count; ++i) {
        auto& catalog = layers[i].catalog;
        for (auto& nested : catalog.nested_packages) {
            for (std::size_t n = 0; n < originals[i].size(); ++n) if (same(nested.input, originals[i][n])) { nested.input = catalog.files[n].path; break; }
            const auto old_output = (layers[i].directory / nested.output).lexically_normal();
            for (std::size_t j = 0; !nested.output.empty() && j < count; ++j) if (!layers[j].directory.empty() && same(old_output, layers[j].directory)) {
                input_paths[j] = nested.input;
                fs::path common;
                const auto& files = layers[j].catalog.files;
                if (!files.empty()) {
                    common = files.front().path.parent_path();
                    for (const auto& file : files) common = common_parent(common, file.path.parent_path());
                }
                nested.output = common.empty() ? fs::path(L".") : common;
                nested.report = reports[j]; break;
            }
            merged.nested_packages.push_back(nested);
        }
    }
    merged.tree_total_bytes = merged.total_size; merged.tree_file_count = merged.files.size();
    merged.notes.push_back(L"精简布局：files 覆盖所有已提取层，路径相对结果根目录；package、originalPath 和 sourceExpression 保留来源。各层脚本与报告仍单独保存。");
    if (fallback || !applications || !complete) merged.notes.push_back(L"存在歧义、未完成内容或名称冲突的布局保留原始相对结构；内层包并列存放。");
    platform::ensure_disk_space(parent, required);
    progress::Scope finalizing(progress::Phase::finalizing, required, L"正在整理精简目录并复核文件");
    io::Output output(parent, input.stem().wstring());
    const auto copy = [&](const Source& source, const fs::path& target) {
        control::checkpoint(); progress::file(target.native());
        io::Input data(source.path);
        require(data.size() == source.size && platform::sha256(data.path()) == source.hash, Status::corrupt, L"暂存文件在目录整理前发生变化。");
        {
            auto file = output.create_file(target); data.copy_to(file.get());
            if (!FlushFileBuffers(file.get())) platform::io_failure(L"保存精简目录文件失败");
        }
        require(platform::sha256(output.full_path(target)) == source.hash, Status::corrupt, L"精简目录文件校验失败。");
    };
    for (std::size_t i = 0; i < sources.size(); ++i) copy(sources[i], merged.files[i].path);
    for (const auto& item : auxiliary) copy(item.source, item.target);
    const auto report = [&](const Catalog& catalog, const fs::path& path) {
        const auto text = platform::utf8(catalog_json(catalog, true));
        auto file = output.create_file(path); platform::write_all(file.get(), std::as_bytes(std::span(text)));
        if (!FlushFileBuffers(file.get())) platform::io_failure(L"保存精简目录映射报告失败");
    };
    for (std::size_t i = 1; i < count; ++i) if (!layers[i].directory.empty()) {
        layers[i].catalog.input = input_paths[i].empty() ? fs::path(L"p" + std::to_wstring(i)) : input_paths[i];
        report(layers[i].catalog, reports[i]);
    }
    report(merged, reports[0]);
    ExtractionResult result;
    result.output = output.commit(); result.primary_output = result.output;
    result.complete = complete && merged.required_paths_resolved() && merged.content_complete;
    result.total_bytes = merged.total_size; result.file_count = merged.files.size();
    result.details = L"已按精简布局保存，原始路径与各层来源见 _extract-report.json。\r\n";
    for (const auto& layer : layers) {
        if (!layer.catalog.runtime_notice.empty()) result.details += layer.catalog.runtime_notice + L"\r\n";
        if (!complete) for (const auto& item : layer.catalog.nested_packages) if (item.status != L"complete" && item.status != L"preserved")
            result.details += item.input.wstring() + L"：" + item.message + L"\r\n";
    }
    return result;
}
}
