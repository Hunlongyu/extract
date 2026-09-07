#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace extract::progress {
enum class Phase { analyzing, extracting, decoding, verifying, scanning, finalizing, preparing };

// 固定容量文本使可选的进度上报不因内存分配失败而影响提取。
struct Text {
    std::array<wchar_t, 512> bytes{};
    std::size_t size = 0;
    void assign(std::wstring_view value) noexcept;
    std::wstring_view view() const noexcept { return {bytes.data(), size}; }
};
struct Snapshot {
    Text package, item;
    Phase phase = Phase::analyzing;
    std::uint64_t completed = 0;
    std::optional<std::uint64_t> total;
};
class Observer {
public:
    virtual ~Observer() = default;
    virtual void update(const Snapshot& value) = 0;
};
class Connection {
public:
    explicit Connection(Observer* observer) noexcept;
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
private:
    Observer* previous_;
};
class Scope {
public:
    explicit Scope(Phase phase, std::optional<std::uint64_t> total = {},
        std::wstring_view item = {}, std::wstring_view package = {}) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    friend void advance(std::uint64_t) noexcept;
    friend void file(std::wstring_view) noexcept;
    friend void pulse() noexcept;
    Scope* previous_;
    Snapshot value_;
};
void advance(std::uint64_t bytes) noexcept;
void file(std::wstring_view name) noexcept;
void pulse() noexcept;
std::wstring_view phase_name(Phase phase) noexcept;
} // namespace extract::progress
