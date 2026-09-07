#pragma once
#include "platform/files.h"

namespace extract::io {
using Bytes = std::span<const std::byte>;

class Reader {
public:
    explicit Reader(Bytes bytes) : bytes_(bytes) {}
    Bytes take(std::size_t size) {
        require(size <= bytes_.size() - position_, Status::corrupt, L"数据长度或偏移越界。");
        auto result = bytes_.subspan(position_, size);
        position_ += size;
        return result;
    }
    void skip(std::size_t size) { (void)take(size); }
    std::uint8_t u8() { return std::to_integer<std::uint8_t>(take(1)[0]); }
    std::uint16_t u16() { const auto a = u8(); return static_cast<std::uint16_t>(a | (u8() << 8)); }
    std::uint32_t u32() { const auto a = u16(); return a | (static_cast<std::uint32_t>(u16()) << 16); }
    std::uint64_t u64() { const auto a = u32(); return a | (static_cast<std::uint64_t>(u32()) << 32); }
    std::size_t remaining() const { return bytes_.size() - position_; }
    std::size_t position() const { return position_; }
private:
    Bytes bytes_;
    std::size_t position_ = 0;
};

inline Bytes slice(Bytes bytes, std::uint64_t start, std::uint64_t length) {
    require(start <= bytes.size() && length <= bytes.size() - start, Status::corrupt, L"输入数据范围越界。");
    return bytes.subspan(static_cast<std::size_t>(start), static_cast<std::size_t>(length));
}

class Input {
public:
    explicit Input(const fs::path& path);
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    const fs::path& path() const { return path_; }
    std::uint64_t size() const { return size_; }
    void read(std::uint64_t offset, std::span<std::byte> destination) const;
    void copy_to(HANDLE destination) const;
    Bytes bytes() const;
    void unmap() const;
private:
    fs::path path_;
    std::vector<platform::Handle> ancestors_;
    platform::Handle file_;
    mutable platform::Handle mapping_;
    mutable const std::byte* view_ = nullptr;
    std::uint64_t size_ = 0;
};
} // namespace extract::io
