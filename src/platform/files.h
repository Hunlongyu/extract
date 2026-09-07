#pragma once
#include "core/types.h"
#include <span>
#include <string_view>
#include <utility>

namespace extract::platform {
class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE); }
        return *this;
    }
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    void reset() noexcept { if (*this) CloseHandle(value_); value_ = INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};

[[noreturn]] void io_failure(const std::wstring& operation, DWORD code = GetLastError());
fs::path absolute_path(const fs::path& path);
std::wstring extended_path(const fs::path& path);
DWORD rename_in_place(HANDLE file, std::wstring_view name, bool replace);
void validate_component(std::wstring_view name);
void validate_relative(const fs::path& path);
bool equal_name(std::wstring_view left, std::wstring_view right);
std::wstring unique_id();
std::string utf8(std::wstring_view text);
void write_all(HANDLE file, std::span<const std::byte> bytes);
std::wstring sha256(const fs::path& path);
std::wstring sha1(const fs::path& path);
std::wstring sha512(const fs::path& path);
std::wstring hash_bytes(std::span<const std::byte> bytes, std::size_t digest_size);
Handle lock_directory(const fs::path& path, bool rename = false);
std::vector<Handle> lock_ancestors(const fs::path& path);
} // namespace extract::platform
