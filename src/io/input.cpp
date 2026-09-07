#include "io/input.h"
#include "platform/log.h"

namespace extract::io {
Input::Input(const fs::path& path, std::uint64_t limit) : path_(platform::absolute_path(path)),
    ancestors_(platform::lock_ancestors(path_.parent_path())) {
    log::Scope step(L"input.open", nullptr, &path_);
    file_ = platform::Handle(CreateFileW(platform::extended_path(path_).c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file_) platform::io_failure(L"无法打开输入文件");
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(file_.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)))
        platform::io_failure(L"无法检查输入文件");
    require((attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0,
            Status::unsafe_path, L"输入必须是普通文件，不能是链接或目录。");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_.get(), &size)) platform::io_failure(L"无法读取输入大小");
    require(size.QuadPart >= 0 && static_cast<std::uint64_t>(size.QuadPart) <= limit,
            Status::limit_exceeded, L"当前读取文件上限为 " + std::to_wstring(limit / (1024 * 1024)) + L" MiB。");
    size_ = static_cast<std::size_t>(size.QuadPart);
    log::detail(log::Level::info, L"input.size", [&] { return L"bytes=" + std::to_wstring(size_); });
    if (size_ == 0) return;
    mapping_ = platform::Handle(CreateFileMappingW(file_.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping_) platform::io_failure(L"无法映射输入文件");
    view_ = static_cast<const std::byte*>(MapViewOfFile(mapping_.get(), FILE_MAP_READ, 0, 0, size_));
    if (!view_) platform::io_failure(L"无法读取输入映射");
}
Input::~Input() { if (view_) UnmapViewOfFile(view_); }
} // namespace extract::io
