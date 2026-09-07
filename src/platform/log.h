#pragma once
#include "core/types.h"
#include <string_view>

namespace extract::log {
enum class Level { info, warning, error };

// 可注入目录和限额，供独立日志测试使用；程序使用 start(version) 的默认策略。
struct Options {
    fs::path primary;
    fs::path fallback;
    std::uint64_t file_bytes = 2ULL * 1024 * 1024;
    std::uint64_t directory_bytes = 10ULL * 1024 * 1024;
    unsigned retention_days = 30;
    std::size_t max_files = 200;
};
void start(std::wstring_view version) noexcept;
void start(const Options& options, std::wstring_view version) noexcept;
void stop(int exit_code) noexcept;
bool enabled() noexcept;
std::wstring location() noexcept;
void write(Level level, std::wstring_view event, std::wstring_view message = {}) noexcept;
void failure(std::wstring_view event, const Failure& error) noexcept;
void exception(std::wstring_view event, const std::exception& error) noexcept;
void verified(const Entry& entry) noexcept;

// 在 noexcept 边界内构造动态消息，日志分配/编码/写盘失败不能影响提取结果。
template<class Builder>
void detail(Level level, std::wstring_view event, Builder&& builder) noexcept {
    if (!enabled()) return;
    const auto error = GetLastError();
    try { write(level, event, builder()); } catch (...) {}
    SetLastError(error);
}

class Scope {
public:
    Scope(std::wstring_view stage, const fs::path* package = nullptr, const fs::path* file = nullptr) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    const Scope* previous;
    std::wstring_view stage;
    const fs::path* package;
    const fs::path* file;
private:
    int exceptions_;
    ULONGLONG started_;
};
} // namespace extract::log
