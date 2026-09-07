#include "codecs/stream.h"
#include "platform/log.h"
#include "core/progress.h"
#include "codecs/nsis_bzip.h"
#include <LzmaDec.h>
#include <Lzma2Dec.h>
#include <zlib.h>
#include <bzlib.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace extract::codecs {
namespace {
constexpr std::size_t dictionary_limit = 256 * 1024 * 1024;
struct Allocator {
    ISzAlloc interface{lz_alloc, lz_free};
    std::size_t used = 0;
    bool exceeded = false;
    void* allocate(std::size_t size) noexcept {
        constexpr auto limit = dictionary_limit + 8 * 1024 * 1024;
        if (size > limit - used) { exceeded = true; return nullptr; }
        auto* block = static_cast<std::max_align_t*>(std::calloc(1, sizeof(std::max_align_t) + size));
        if (!block) return nullptr;
        std::memcpy(block, &size, sizeof(size));
        used += size;
        return block + 1;
    }
    void release(void* pointer) noexcept {
        if (!pointer) return;
        auto* block = static_cast<std::max_align_t*>(pointer) - 1;
        std::size_t size = 0;
        std::memcpy(&size, block, sizeof(size));
        used -= size;
        std::free(block);
    }
    static void* lz_alloc(ISzAllocPtr p, std::size_t size) noexcept {
        return reinterpret_cast<Allocator*>(const_cast<ISzAlloc*>(p))->allocate(size);
    }
    static void lz_free(ISzAllocPtr p, void* pointer) noexcept {
        reinterpret_cast<Allocator*>(const_cast<ISzAlloc*>(p))->release(pointer);
    }
    static void* z_alloc(void* p, unsigned count, unsigned size) noexcept {
        if (size && count > (std::numeric_limits<std::size_t>::max)() / size) return nullptr;
        return static_cast<Allocator*>(p)->allocate(static_cast<std::size_t>(count) * size);
    }
    static void z_free(void* p, void* pointer) noexcept { static_cast<Allocator*>(p)->release(pointer); }
    static void* bz_alloc(void* p, int count, int size) noexcept {
        if (count < 0 || size < 0) return nullptr;
        if (size && static_cast<unsigned>(count) > (std::numeric_limits<std::size_t>::max)() / static_cast<unsigned>(size)) return nullptr;
        return static_cast<Allocator*>(p)->allocate(static_cast<std::size_t>(count) * static_cast<unsigned>(size));
    }
};
static_assert(offsetof(Allocator, interface) == 0);
}

std::uint32_t crc32(io::Bytes bytes) {
    progress::Scope stage(progress::Phase::verifying, bytes.size(), L"安装包 CRC-32");
    uLong result = 0;
    while (!bytes.empty()) {
        const auto count = (std::min)(bytes.size(), std::size_t{65536});
        result = ::crc32_z(result, reinterpret_cast<const Bytef*>(bytes.data()), count);
        bytes = bytes.subspan(count); progress::advance(count);
    }
    return static_cast<std::uint32_t>(result);
}

struct Stream::Impl {
    Compression method;
    io::Bytes input;
    std::size_t position = 0;
    bool ended = false, initialized = false;
    Allocator allocator;
    CLzmaDec lzma{};
    CLzma2Dec lzma2{};
    z_stream z{};
    bz_stream bz{};
    void* nsis_bz = nullptr;

    Impl(Compression m, io::Bytes bytes) : method(m), input(bytes) {}
    void init() {
        if (method == Compression::lzma1) {
            require(input.size() >= 5, Status::corrupt, L"LZMA 属性缺失。");
            CLzmaProps props{};
            const auto* data = reinterpret_cast<const Byte*>(input.data());
            require(LzmaProps_Decode(&props, data, 5) == SZ_OK, Status::corrupt, L"LZMA 属性无效。");
            require(props.dicSize <= dictionary_limit, Status::limit_exceeded, L"LZMA 字典超过 256 MiB。");
            LzmaDec_Construct(&lzma);
            const auto result = LzmaDec_Allocate(&lzma, data, 5, &allocator.interface);
            initialized = true;
            require(result == SZ_OK, result == SZ_ERROR_MEM ? Status::limit_exceeded : Status::corrupt, L"LZMA 初始化失败。");
            LzmaDec_Init(&lzma);
            position = 5;
        } else if (method == Compression::lzma2) {
            require(!input.empty(), Status::corrupt, L"LZMA2 属性缺失。");
            const auto prop = std::to_integer<Byte>(input[0]);
            require(prop <= 40, Status::corrupt, L"LZMA2 字典属性无效。");
            const std::uint64_t dictionary = prop == 40 ? 0xffffffffULL : static_cast<std::uint64_t>(2 | (prop & 1)) << (prop / 2 + 11);
            require(dictionary <= dictionary_limit, Status::limit_exceeded, L"LZMA2 字典超过 256 MiB。");
            Lzma2Dec_Construct(&lzma2);
            const auto result = Lzma2Dec_Allocate(&lzma2, prop, &allocator.interface);
            initialized = true;
            require(result == SZ_OK, result == SZ_ERROR_MEM ? Status::limit_exceeded : Status::corrupt, L"LZMA2 初始化失败。");
            Lzma2Dec_Init(&lzma2);
            position = 1;
        } else if (method == Compression::deflate || method == Compression::raw_deflate) {
            z.zalloc = Allocator::z_alloc; z.zfree = Allocator::z_free; z.opaque = &allocator;
            const auto result = inflateInit2(&z, method == Compression::raw_deflate ? -15 : 15);
            initialized = result == Z_OK;
            require(initialized, Status::limit_exceeded, L"Deflate 初始化失败。");
        } else if (method == Compression::bzip2) {
            bz.bzalloc = Allocator::bz_alloc; bz.bzfree = Allocator::z_free; bz.opaque = &allocator;
            const auto result = BZ2_bzDecompressInit(&bz, 0, 0);
            initialized = result == BZ_OK;
            require(initialized, Status::limit_exceeded, L"bzip2 初始化失败。");
        } else if (method == Compression::nsis_bzip2) {
            nsis_bz = extract_nsis_bzip_create();
            initialized = nsis_bz != nullptr;
            require(initialized, Status::limit_exceeded, L"NSIS bzip2 初始化失败。");
        }
    }
    ~Impl() {
        if (!initialized) return;
        if (method == Compression::lzma1) LzmaDec_Free(&lzma, &allocator.interface);
        else if (method == Compression::lzma2) Lzma2Dec_Free(&lzma2, &allocator.interface);
        else if (method == Compression::deflate || method == Compression::raw_deflate) inflateEnd(&z);
        else if (method == Compression::bzip2) BZ2_bzDecompressEnd(&bz);
        else if (method == Compression::nsis_bzip2) extract_nsis_bzip_destroy(nsis_bz);
    }
    std::size_t read(std::span<std::byte> output) {
        if (ended || output.empty()) return 0;
        if (method == Compression::stored) {
            const auto size = (std::min)(output.size(), input.size() - position);
            if (size) std::memcpy(output.data(), input.data() + position, size);
            position += size;
            ended = position == input.size();
            return size;
        }
        std::size_t written = 0;
        while (written < output.size() && !ended) {
            auto remaining = input.subspan(position);
            auto destination = output.subspan(written);
            SizeT used = (std::min)(remaining.size(), static_cast<std::size_t>((std::numeric_limits<unsigned>::max)()));
            SizeT produced = (std::min)(destination.size(), static_cast<std::size_t>((std::numeric_limits<unsigned>::max)()));
            if (method == Compression::lzma1 || method == Compression::lzma2) {
                ELzmaStatus status{};
                const auto result = method == Compression::lzma1
                    ? LzmaDec_DecodeToBuf(&lzma, reinterpret_cast<Byte*>(destination.data()), &produced,
                        reinterpret_cast<const Byte*>(remaining.data()), &used, LZMA_FINISH_ANY, &status)
                    : Lzma2Dec_DecodeToBuf(&lzma2, reinterpret_cast<Byte*>(destination.data()), &produced,
                        reinterpret_cast<const Byte*>(remaining.data()), &used, LZMA_FINISH_ANY, &status);
                require(result == SZ_OK, result == SZ_ERROR_MEM ? Status::limit_exceeded : Status::corrupt, L"LZMA 数据解码失败。");
                ended = status == LZMA_STATUS_FINISHED_WITH_MARK;
            } else if (method == Compression::deflate || method == Compression::raw_deflate) {
                z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(remaining.data())); z.avail_in = static_cast<uInt>(used);
                z.next_out = reinterpret_cast<Bytef*>(destination.data()); z.avail_out = static_cast<uInt>(produced);
                const auto result = inflate(&z, Z_NO_FLUSH);
                used -= z.avail_in; produced -= z.avail_out;
                require(result == Z_OK || result == Z_STREAM_END || result == Z_BUF_ERROR,
                        result == Z_MEM_ERROR ? Status::limit_exceeded : Status::corrupt, L"Deflate 数据解码失败。");
                ended = result == Z_STREAM_END;
            } else if (method == Compression::nsis_bzip2) {
                auto consumed = static_cast<unsigned>(used), count = static_cast<unsigned>(produced);
                const auto result = extract_nsis_bzip_read(nsis_bz,
                    reinterpret_cast<const unsigned char*>(remaining.data()), &consumed,
                    reinterpret_cast<unsigned char*>(destination.data()), &count);
                require(result >= 0, Status::corrupt, L"NSIS bzip2 数据解码失败。");
                used = consumed; produced = count; ended = result == 1;
            } else {
                bz.next_in = reinterpret_cast<char*>(const_cast<std::byte*>(remaining.data())); bz.avail_in = static_cast<unsigned>(used);
                bz.next_out = reinterpret_cast<char*>(destination.data()); bz.avail_out = static_cast<unsigned>(produced);
                const auto result = BZ2_bzDecompress(&bz);
                used -= bz.avail_in; produced -= bz.avail_out;
                require(result == BZ_OK || result == BZ_STREAM_END,
                        result == BZ_MEM_ERROR ? Status::limit_exceeded : Status::corrupt, L"bzip2 数据解码失败。");
                ended = result == BZ_STREAM_END;
            }
            require(ended || used != 0 || produced != 0, Status::corrupt, L"压缩数据被截断或解码停滞。");
            position += used; written += produced;
        }
        if (ended) require(position == input.size(), Status::corrupt, L"压缩流结束后存在未声明数据。");
        return written;
    }
};
Stream::Stream(Compression method, io::Bytes input) : impl_(std::make_unique<Impl>(method, input)) {
    log::Scope step(L"codec.initialize");
    log::detail(log::Level::info, L"codec.stream", [&] {
        constexpr std::wstring_view names[] = {L"stored", L"Deflate", L"bzip2", L"LZMA", L"LZMA2", L"raw Deflate", L"NSIS bzip2"};
        const auto index = static_cast<std::size_t>(method);
        return std::wstring(index < std::size(names) ? names[index] : L"unknown") + L"; compressedBytes=" + std::to_wstring(input.size());
    });
    impl_->init();
}
Stream::~Stream() = default;
void Stream::read_exact(std::span<std::byte> output) {
    require(impl_->read(output) == output.size(), Status::corrupt, L"压缩流未提供声明的文件内容。");
}
std::size_t Stream::read(std::span<std::byte> output) { return impl_->read(output); }
std::uint64_t Stream::consumed() const noexcept { return impl_->position; }
void Stream::finish() {
    std::array<std::byte, 1> extra{};
    require(impl_->read(extra) == 0, Status::corrupt, L"压缩流包含超出文件清单的数据。");
}
std::vector<std::byte> decode_bounded(Compression method, io::Bytes input, std::size_t limit) {
    progress::Scope stage(progress::Phase::preparing, input.size(), L"正在解压目录元数据，按压缩数据读取量");
    Stream decoder(method, input);
    std::uint64_t consumed = 0;
    std::vector<std::byte> output;
    std::array<std::byte, 65536> chunk{};
    for (;;) {
        const auto size = decoder.read(chunk);
        progress::advance(decoder.consumed() - consumed); consumed = decoder.consumed();
        if (!size) break;
        require(size <= limit - output.size(), Status::limit_exceeded, L"解压后的元数据超过大小上限。");
        output.insert(output.end(), chunk.begin(), chunk.begin() + size);
    }
    return output;
}
} // namespace extract::codecs
