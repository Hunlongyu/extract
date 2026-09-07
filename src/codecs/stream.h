#pragma once
#include "io/input.h"
#include <memory>

namespace extract::codecs {
enum class Compression { stored, deflate, bzip2, lzma1, lzma2, raw_deflate, nsis_bzip2 };
std::uint32_t crc32(io::Bytes bytes);
class Stream {
public:
    Stream(Compression method, io::Bytes input);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    void read_exact(std::span<std::byte> output);
    std::size_t read(std::span<std::byte> output);
    void finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::vector<std::byte> decode_bounded(Compression method, io::Bytes input, std::size_t limit);
} // namespace extract::codecs
