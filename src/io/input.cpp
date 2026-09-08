#include "io/input.h"
#include "platform/log.h"
#include "core/control.h"
#include <algorithm>

namespace extract::io {
Input::Input(const fs::path& path) : path_(platform::absolute_path(path)),
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
    require(size.QuadPart >= 0, Status::corrupt, L"输入文件大小无效。");
    size_ = static_cast<std::uint64_t>(size.QuadPart);
    log::detail(log::Level::info, L"input.size", [&] { return L"bytes=" + std::to_wstring(size_); });
}
void Input::read(std::uint64_t offset, std::span<std::byte> destination) const {
    control::checkpoint();
    require(offset <= size_ && destination.size() <= size_ - offset, Status::corrupt, L"文件读取范围越界。");
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file_.get(), position, nullptr, FILE_BEGIN)) platform::io_failure(L"无法定位输入文件");
    while (!destination.empty()) {
        const auto count = static_cast<DWORD>((std::min)(destination.size(), std::size_t{1 << 20}));
        DWORD received = 0;
        if (!ReadFile(file_.get(), destination.data(), count, &received, nullptr)) platform::io_failure(L"无法读取输入文件");
        require(received != 0, Status::corrupt, L"输入文件提前结束。");
        destination = destination.subspan(received);
    }
}
void Input::copy_to(HANDLE destination) const {
    std::array<std::byte, 65536> buffer{};
    for (std::uint64_t offset = 0; offset < size_;) {
        const auto count = static_cast<std::size_t>((std::min)(size_ - offset, static_cast<std::uint64_t>(buffer.size())));
        auto part = std::span(buffer).first(count);
        read(offset, part); platform::write_payload(destination, part); offset += count;
    }
}
Bytes Input::bytes() const {
    if (size_ == 0) return {};
    require(size_ <= (std::numeric_limits<std::size_t>::max)(), Status::limit_exceeded,
        L"当前解析器需要连续地址空间，此文件超出当前程序架构的范围；请使用 x64 或 ARM64 版本。");
    if (view_) return {view_, static_cast<std::size_t>(size_)};
    mapping_ = platform::Handle(CreateFileMappingW(file_.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping_) platform::io_failure(L"无法映射输入文件");
    view_ = static_cast<const std::byte*>(MapViewOfFile(mapping_.get(), FILE_MAP_READ, 0, 0, static_cast<std::size_t>(size_)));
    if (!view_) platform::io_failure(L"无法映射输入文件，连续地址空间或系统内存不足；32 位程序请改用 x64 或 ARM64");
    return {view_, static_cast<std::size_t>(size_)};
}
void Input::unmap() const {
    if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
    mapping_.reset();
}
Input::~Input() { unmap(); }
} // namespace extract::io
