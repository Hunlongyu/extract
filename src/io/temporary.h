#pragma once
#include "io/input.h"
#include "io/output.h"

namespace extract::io {
inline fs::path temporary_directory() {
    std::wstring path(32768, L'\0');
    const auto count = GetTempPathW(static_cast<DWORD>(path.size()), path.data());
    if (!count || count >= path.size()) platform::io_failure(L"无法获取临时目录");
    path.resize(count); return path;
}

// 保持原始句柄及父目录锁，不重开缓存文件。大载荷不累计在 vector 中。
class TemporaryFile {
public:
    explicit TemporaryFile(std::wstring name) : output_(temporary_directory(), std::move(name)),
        file_(output_.create_file(L"data.bin", true)) {}
    ~TemporaryFile() { if (view_) UnmapViewOfFile(view_); }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    void append(Bytes bytes) {
        require(!mapping_, Status::internal_error, L"不能修改已映射的缓存。");
        const auto next = checked_size_sum(size_, bytes.size());
        if (size_ >= checked_until_) {
            // 只检查下一批数据的空间，不为小尾块强加固定磁盘余量。
            platform::ensure_disk_space(output_.full_path(L"").parent_path(), bytes.size());
            checked_until_ = checked_size_sum(size_, 64 * 1024 * 1024);
        }
        platform::write_all(file_.get(), bytes); size_ = next;
    }
    Bytes bytes() {
        if (!size_) return {};
        require(size_ <= (std::numeric_limits<std::size_t>::max)(), Status::limit_exceeded,
            L"缓存需要连续地址空间，已超过当前程序架构的范围；请使用 x64 或 ARM64 版本。");
        if (!mapping_) {
            mapping_ = platform::Handle(CreateFileMappingW(file_.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
            if (!mapping_) platform::io_failure(L"无法建立临时缓存映射");
            view_ = static_cast<const std::byte*>(MapViewOfFile(mapping_.get(), FILE_MAP_READ, 0, 0, static_cast<std::size_t>(size_)));
            if (!view_) { const auto error = GetLastError(); mapping_.reset(); platform::io_failure(L"无法映射临时缓存，内存或连续地址空间不足", error); }
        }
        return {view_, static_cast<std::size_t>(size_)};
    }
private:
    Output output_;
    platform::Handle file_, mapping_;
    const std::byte* view_ = nullptr;
    std::uint64_t size_ = 0, checked_until_ = 0;
};
} // namespace extract::io
