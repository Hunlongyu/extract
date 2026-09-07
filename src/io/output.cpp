#include "io/output.h"
#include "platform/log.h"

namespace extract::io {
Output::Output(const fs::path& parent, std::wstring stem)
    : parent_(platform::absolute_path(parent)), stem_(std::move(stem)), ancestors_(platform::lock_ancestors(parent_)) {
    // 给最终目录的序号后缀预留空间。
    require(stem_.size() <= 220, Status::limit_exceeded, L"安装包名称过长，请先缩短文件名。");
    platform::validate_component(stem_);
    staging_ = parent_ / (L".extract-" + platform::unique_id() + L".tmp");
    log::detail(log::Level::info, L"output.staging", [&] { return staging_.wstring(); });
    if (!CreateDirectoryW(platform::extended_path(staging_).c_str(), nullptr)) platform::io_failure(L"无法创建临时目录");
    try {
        directories_.push_back({staging_, platform::lock_directory(staging_, true)});
    } catch (...) {
        RemoveDirectoryW(platform::extended_path(staging_).c_str());
        throw;
    }
}

Output::~Output() {
    if (committed_) return;
    if (!cleanup_safe_) {
        log::detail(log::Level::warning, L"output.preserved", [&] { return staging_.wstring(); });
        return;
    }
    const bool failed = std::uncaught_exceptions() > 0;
    log::detail(failed ? log::Level::warning : log::Level::info, failed ? L"output.rollback" : L"output.cleanup", [&] { return staging_.wstring(); });
    try {
        // 只清理本任务实际创建的项目，不递归遍历可能被插入的未知内容。
        for (auto it = files_.rbegin(); it != files_.rend(); ++it) DeleteFileW(platform::extended_path(*it).c_str());
        for (auto it = directories_.rbegin(); it != directories_.rend(); ++it) {
            it->lock.reset();
            RemoveDirectoryW(platform::extended_path(it->path).c_str());
        }
    } catch (...) { /* 内存不足等情况下保留临时文件，析构不能再次抛出。 */ }
}

platform::Handle Output::create_file(const fs::path& relative, bool read_access) {
    log::Scope step(L"file.create", nullptr, &relative);
    platform::validate_relative(relative);
    fs::path current = staging_;
    for (const auto& part : relative.parent_path()) {
        current /= part;
        bool known = false;
        for (const auto& directory : directories_) {
            if (platform::equal_name(directory.path.wstring(), current.wstring())) { known = true; break; }
        }
        if (!known) {
            // 预先保存清理信息，避免分配失败遗留未登记的目录。
            directories_.push_back({current, platform::Handle{}});
            if (!CreateDirectoryW(platform::extended_path(current).c_str(), nullptr)) {
                directories_.pop_back();
                platform::io_failure(L"无法创建输出子目录");
            }
            directories_.back().lock = platform::lock_directory(current);
        }
    }
    const auto path = staging_ / relative;
    files_.push_back(path);
    platform::Handle file(CreateFileW(platform::extended_path(path).c_str(), GENERIC_WRITE | (read_access ? GENERIC_READ : 0),
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file) { files_.pop_back(); platform::io_failure(L"无法创建输出文件"); }
    return file;
}

fs::path Output::commit() {
    log::Scope step(L"output.commit", nullptr, &staging_);
    // Windows 重命名父目录前必须释放子目录锁。此后不再写载荷或按路径清理；
    // 提交失败时保留临时结果，避免释放锁后的清理竞争跟随被替换的路径。
    cleanup_safe_ = false;
    for (std::size_t i = 1; i < directories_.size(); ++i) directories_[i].lock.reset();
    for (unsigned index = 1; index < 10000; ++index) {
        const auto name = stem_ + L"_extracted" + (index == 1 ? L"" : L" (" + std::to_wstring(index) + L")");
        const fs::path destination = parent_ / name;
        const std::wstring native = platform::extended_path(destination);
        if (GetFileAttributesW(native.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        DWORD error = ERROR_SUCCESS;
        for (unsigned attempt = 0; attempt < 21; ++attempt) {
            error = platform::rename_in_place(directories_.front().lock.get(), name, false);
            if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
            // 刚写出的文件可能被索引器等短暂打开；仅重试同一目录句柄，不修改权限。
            if (attempt == 20 || GetFileAttributesW(native.c_str()) != INVALID_FILE_ATTRIBUTES) break;
            Sleep(100);
        }
        if (error == ERROR_SUCCESS) {
            committed_ = true;
            log::detail(log::Level::info, L"output.committed", [&] { return destination.wstring(); });
            return destination;
        }
        if (GetFileAttributesW(native.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
            platform::io_failure(L"提交输出目录失败，临时结果保留在：" + staging_.wstring(), error);
    }
    throw Failure(Status::io_error, L"同名输出目录数量过多。");
}
} // namespace extract::io
