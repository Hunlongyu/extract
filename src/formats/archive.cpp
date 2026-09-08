#include "formats/archive.h"
#include "platform/log.h"
#include "core/progress.h"
#include "codecs/stream.h"
#include "io/output.h"
#include <7z.h>
#include <7zCrc.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <limits>

namespace extract::formats {
namespace {
constexpr std::size_t metadata_limit = 64 * 1024 * 1024;
void initialize_crc() {
    static std::once_flag initialized;
    std::call_once(initialized, [] { CrcGenerateTable(); });
}
std::uint32_t u32(io::Bytes b, std::uint64_t p) { return io::Reader(io::slice(b, p, 4)).u32(); }
bool signature(io::Bytes b, std::size_t p, std::string_view value) {
    return p <= b.size() && value.size() <= b.size() - p && std::memcmp(b.data() + p, value.data(), value.size()) == 0;
}
std::optional<ArchiveKind> kind_at(io::Bytes b, std::size_t p) {
    if (signature(b, p, "7z\xbc\xaf\x27\x1c")) return ArchiveKind::seven_zip;
    if (signature(b, p, "PK\x03\x04") || signature(b, p, "PK\x05\x06")) return ArchiveKind::zip;
    return {};
}
std::wstring decode_name(io::Bytes bytes, unsigned page) {
    require(!bytes.empty() && bytes.size() <= 32768, Status::corrupt, L"归档文件名长度无效。");
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    const auto flags = page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0U;
    const auto length = MultiByteToWideChar(page, flags, data, static_cast<int>(bytes.size()), nullptr, 0);
    require(length > 0, Status::corrupt, L"归档文件名编码无效。");
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    require(MultiByteToWideChar(page, flags, data, static_cast<int>(bytes.size()), text.data(), length) == length,
        Status::corrupt, L"无法转换归档文件名。");
    require(text.find(L'\0') == text.npos, Status::unsafe_path, L"归档文件名含空字符。");
    return text;
}
void attributes(std::uint32_t value) {
    require((value & FILE_ATTRIBUTE_REPARSE_POINT) == 0, Status::unsafe_path, L"不提取归档中的重解析点。");
    const auto mode = (value >> 16) & 0170000U;
    require(mode == 0 || mode == 0100000 || mode == 0040000, Status::unsafe_path, L"不提取归档中的符号链接或特殊文件。");
}
fs::path archive_path(std::wstring text, bool directory) {
    if (directory && !text.empty() && (text.back() == L'/' || text.back() == L'\\')) text.pop_back();
    fs::path path(text);
    platform::validate_relative(path);
    return path;
}
void sdk_result(SRes result) {
    if (result == SZ_OK) return;
    if (result == SZ_ERROR_MEM) throw Failure(Status::limit_exceeded, L"7z 元数据或解码辅助内存超过上限。");
    if (result == SZ_ERROR_UNSUPPORTED) throw Failure(Status::unsupported, L"尚未支持此 7z 压缩方法、过滤器组合或加密结构。");
    throw Failure(Status::corrupt, L"7z 结构、数据或 CRC 校验失败，代码 " + std::to_wstring(result) + L"。");
}

// C 回调不抛 C++ 异常；元数据和辅助解码分配分别有累计上限。
struct BudgetAllocator {
    ISzAlloc api;
    std::size_t used = 0, limit;
    explicit BudgetAllocator(std::size_t maximum) : api{allocate, release}, limit(maximum) {}
    static BudgetAllocator& self(ISzAllocPtr p) { return *reinterpret_cast<BudgetAllocator*>(const_cast<ISzAlloc*>(p)); }
    union Allocation { std::max_align_t alignment; std::size_t size; };
    static void* allocate(ISzAllocPtr p, std::size_t size) noexcept {
        auto& a = self(p);
        if (!size || size > a.limit - a.used) return nullptr;
        auto* block = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + size));
        if (!block) return nullptr;
        block->size = size; a.used += size; return block + 1;
    }
    static void release(ISzAllocPtr p, void* data) noexcept {
        if (!data) return;
        auto* block = static_cast<Allocation*>(data) - 1;
        self(p).used -= block->size; std::free(block);
    }
};
struct MemoryStream {
    ILookInStream api{look, skip, read, seek};
    io::Bytes bytes;
    std::size_t position = 0;
    explicit MemoryStream(io::Bytes value) : bytes(value) {}
    static MemoryStream& self(ILookInStreamPtr p) { return *reinterpret_cast<MemoryStream*>(const_cast<ILookInStream*>(p)); }
    static SRes look(ILookInStreamPtr p, const void** buffer, size_t* size) noexcept {
        auto& s = self(p); *size = (std::min)(*size, s.bytes.size() - s.position);
        *buffer = s.bytes.data() + s.position; return SZ_OK;
    }
    static SRes skip(ILookInStreamPtr p, size_t size) noexcept {
        auto& s = self(p); if (size > s.bytes.size() - s.position) return SZ_ERROR_INPUT_EOF;
        s.position += size; progress::pulse(); return SZ_OK;
    }
    static SRes read(ILookInStreamPtr p, void* buffer, size_t* size) noexcept {
        const void* source = nullptr; look(p, &source, size);
        if (*size) std::memcpy(buffer, source, *size);
        return skip(p, *size);
    }
    static SRes seek(ILookInStreamPtr p, Int64* position, ESzSeek origin) noexcept {
        auto& s = self(p);
        const auto base = origin == SZ_SEEK_SET ? 0LL : (origin == SZ_SEEK_CUR ? static_cast<Int64>(s.position) : static_cast<Int64>(s.bytes.size()));
        if (origin != SZ_SEEK_SET && origin != SZ_SEEK_CUR && origin != SZ_SEEK_END) return SZ_ERROR_PARAM;
        if (*position < -base || *position > static_cast<Int64>(s.bytes.size()) - base) return SZ_ERROR_INPUT_EOF;
        *position += base; s.position = static_cast<std::size_t>(*position); return SZ_OK;
    }
};
class MappedFolder {
public:
    explicit MappedFolder(std::size_t size) : output_(temporary(), L"7z-cache"), size_(size) {
        file_ = output_.create_file(L"folder.bin", true);
        if (!size) return;
        platform::ensure_disk_space(output_.full_path(L"folder.bin").parent_path(), size);
        LARGE_INTEGER length{}; length.QuadPart = static_cast<LONGLONG>(size);
        if (!SetFilePointerEx(file_.get(), length, nullptr, FILE_BEGIN) || !SetEndOfFile(file_.get())) platform::io_failure(L"无法分配 7z 磁盘缓存");
        mapping_ = platform::Handle(CreateFileMappingW(file_.get(), nullptr, PAGE_READWRITE, 0, 0, nullptr));
        if (!mapping_) platform::io_failure(L"无法建立 7z 缓存映射");
        data_ = static_cast<Byte*>(MapViewOfFile(mapping_.get(), FILE_MAP_WRITE, 0, 0, size));
        if (!data_) platform::io_failure(L"无法读取 7z 缓存映射");
    }
    ~MappedFolder() { if (data_) UnmapViewOfFile(data_); }
    Byte* data() { return size_ ? data_ : &empty_; }
    std::size_t size() const { return size_; }
private:
    static fs::path temporary() {
        std::wstring value(32768, L'\0'); const auto n = GetTempPathW(static_cast<DWORD>(value.size()), value.data());
        if (!n || n >= value.size()) platform::io_failure(L"无法读取临时目录");
        value.resize(n); return value;
    }
    io::Output output_;
    platform::Handle file_, mapping_;
    std::size_t size_;
    Byte* data_ = nullptr;
    Byte empty_ = 0;
};
struct Seven {
    BudgetAllocator main{metadata_limit}, temporary{256 * 1024 * 1024};
    CSzArEx database{};
    MemoryStream stream;
    explicit Seven(io::Bytes bytes) : stream(bytes) {
        SzArEx_Init(&database);
        const auto result = SzArEx_Open(&database, &stream.api, &main.api, &temporary.api);
        if (result != SZ_OK) { SzArEx_Free(&database, &main.api); sdk_result(result); }
    }
    ~Seven() { SzArEx_Free(&database, &main.api); }
};
struct ZipFile { std::uint64_t data, packed; codecs::Compression method; std::uint64_t local_header_size; };
}

std::optional<ArchiveLocation> archive_probe(io::Bytes bytes) {
    if (const auto k = kind_at(bytes, 0)) return ArchiveLocation{*k, 0};
    if (!signature(bytes, 0, "MZ") || bytes.size() < 64) return {};
    const auto pe = u32(bytes, 60);
    io::Reader header(io::slice(bytes, pe, 24));
    if (header.u32() != 0x4550) return {};
    header.skip(2); const auto count = header.u16(); header.skip(12); const auto optional = header.u16();
    require(count > 0 && count <= 96, Status::corrupt, L"SFX PE 节数量无效。");
    const auto start = static_cast<std::uint64_t>(pe) + 24 + optional;
    std::uint64_t end = start + count * 40ULL;
    io::Reader sections(io::slice(bytes, start, count * 40ULL));
    for (unsigned i = 0; i < count; ++i) {
        sections.skip(16); const auto size = sections.u32(), offset = sections.u32(); sections.skip(16);
        (void)io::slice(bytes, offset, size); if (size) end = (std::max)(end, static_cast<std::uint64_t>(offset) + size);
    }
    if (end > 16 * 1024 * 1024) return {};
    const auto stop = (std::min)(bytes.size(), static_cast<std::size_t>(end) + 65536);
    for (auto p = static_cast<std::size_t>(end); p < stop; ++p)
        if (const auto k = kind_at(bytes, p)) return ArchiveLocation{*k, p};
    return {};
}

struct ArchivePackage::Impl {
    std::unique_ptr<io::Input> input;
    io::Bytes bytes;
    Catalog catalog;
    std::unique_ptr<Seven> seven;
    std::vector<std::uint32_t> indices;
    std::vector<ZipFile> zip;
    std::function<void(std::size_t, std::uint64_t, io::Bytes)> verifier;
    explicit Impl(const fs::path& path) : input(std::make_unique<io::Input>(path)), bytes(input->bytes()) {
        initialize_crc();
        const auto location = archive_probe(bytes);
        require(location.has_value(), Status::unsupported, L"未识别为 ZIP/7z 或受支持的 SFX。");
        catalog.input = input->path();
        if (location->kind == ArchiveKind::seven_zip) read_seven(location->offset);
        else read_zip(location->offset);
        plan_paths(catalog);
        catalog.notes.push_back(L"静态归档提取；不执行 SFX 配置、脚本、链接或输出文件。空目录不单独恢复。");
        if (location->offset) catalog.notes.push_back(L"PE 自解压外壳附加区偏移：" + std::to_wstring(location->offset));
    }
    Impl(const fs::path& path, io::Bytes zip_bytes) : bytes(zip_bytes) {
        initialize_crc();
        require(kind_at(bytes, 0) == ArchiveKind::zip, Status::corrupt, L"内嵌载荷缺少 ZIP 签名。");
        catalog.input = path;
        read_zip(0);
        plan_paths(catalog);
    }
    void add(Entry file) {
        require(file.size <= max_file_bytes - catalog.total_size,
            Status::limit_exceeded, L"归档总输出大小溢出。");
        catalog.total_size += file.size; catalog.files.push_back(std::move(file));
    }
    void read_seven(std::size_t offset) {
        catalog.format = L"7z"; catalog.format_version = L"0.x";
        seven = std::make_unique<Seven>(bytes.subspan(offset));
        const auto& db = seven->database;
        std::uint64_t expanded = 0;
        for (std::uint32_t folder = 0; folder < db.db.NumFolders; ++folder) {
            const auto size = SzAr_GetFolderUnpackSize(&db.db, folder);
            require(size <= max_file_bytes - expanded, Status::limit_exceeded, L"7z 固实块累计展开超出 64 位文件范围。");
            require(size <= (std::numeric_limits<std::size_t>::max)(), Status::limit_exceeded,
                L"7z 固实块超过当前程序架构的地址空间范围，请使用 x64 或 ARM64 版本。");
            expanded += size;
        }
        for (std::uint32_t i = 0; i < db.NumFiles; ++i) {
            const auto length = SzArEx_GetFileNameUtf16(&db, i, nullptr);
            require(length > 1 && length <= 16001, Status::corrupt, L"7z 文件名长度无效。");
            std::vector<UInt16> name(length); SzArEx_GetFileNameUtf16(&db, i, name.data());
            std::wstring text(name.begin(), name.end() - 1);
            require(text.find(L'\0') == text.npos, Status::unsafe_path, L"7z 文件名含空字符。");
            if (SzBitWithVals_Check(&db.Attribs, i)) attributes(db.Attribs.Vals[i]);
            const bool directory = SzArEx_IsDir(&db, i);
            const auto path = archive_path(text, directory);
            if (directory) continue;
            Entry file; file.id = L"7z-" + std::to_wstring(i); file.path = file.original_path = path;
            file.size = SzArEx_GetFileSize(&db, i); file.source_expression = text;
            if (SzBitWithVals_Check(&db.CRCs, i)) file.expected_crc32 = db.CRCs.Vals[i];
            add(std::move(file)); indices.push_back(i);
        }
        catalog.package_crc_present = catalog.package_crc_verified = true;
    }
    void read_zip(std::size_t archive_offset);
    fs::path extract(const fs::path& parent) {
        io::Output output(parent.empty() ? catalog.input.parent_path() : parent, catalog.input.stem().wstring());
        std::unique_ptr<MappedFolder> folder_data;
        std::uint32_t current_folder = 0xffffffffU;
        std::array<std::byte, 65536> buffer{};
        for (std::size_t i = 0; i < catalog.files.size(); ++i) {
            auto& entry = catalog.files[i];
            progress::file(entry.path.native());
            log::Scope step(L"file.extract", nullptr, &entry.path);
            auto file = output.create_file(entry.path);
            UInt32 crc = CRC_INIT_VAL;
            if (seven) {
                const auto& db = seven->database; const auto index = indices[i]; const auto folder = db.FileToFolder[index];
                if (folder != 0xffffffffU) {
                    if (folder != current_folder) {
                        log::Scope folder_step(L"7z.decode_folder");
                        progress::Scope stage(progress::Phase::decoding, {}, L"7z 固实数据块");
                        folder_data.reset();
                        folder_data = std::make_unique<MappedFolder>(static_cast<std::size_t>(SzAr_GetFolderUnpackSize(&db.db, folder)));
                        sdk_result(SzAr_DecodeFolder(&db.db, folder, &seven->stream.api, db.dataPos,
                            folder_data->data(), folder_data->size(), &seven->temporary.api));
                        current_folder = folder;
                    }
                    const auto base = db.UnpackPositions[db.FolderToFile[folder]];
                    require(db.UnpackPositions[index] >= base, Status::corrupt, L"7z 文件偏移无效。");
                    const auto offset = db.UnpackPositions[index] - base;
                    const auto data = io::slice({reinterpret_cast<const std::byte*>(folder_data->data()), folder_data->size()}, offset, entry.size);
                    for (std::size_t offset_in_file = 0; offset_in_file < data.size();) {
                        const auto part = data.subspan(offset_in_file, (std::min)(buffer.size(), data.size() - offset_in_file));
                        crc = CrcUpdate(crc, part.data(), part.size()); platform::write_payload(file.get(), part);
                        offset_in_file += part.size();
                    }
                } else require(entry.size == 0, Status::corrupt, L"7z 非空文件缺少数据流。");
            } else {
                const auto& payload = zip[i]; codecs::Stream stream(payload.method, io::slice(bytes, payload.data, payload.packed));
                std::uint64_t remaining = entry.size;
                while (remaining) {
                    const auto n = static_cast<std::size_t>((std::min)(remaining, static_cast<std::uint64_t>(buffer.size())));
                    auto part = std::span(buffer).first(n); stream.read_exact(part);
                    if (verifier) verifier(i, entry.size - remaining, part);
                    crc = CrcUpdate(crc, part.data(), part.size()); platform::write_payload(file.get(), part); remaining -= n;
                }
                stream.finish();
                if (verifier) verifier(i, entry.size, {});
                entry.block_hash_verified = !entry.block_hash_algorithm.empty();
            }
            if (entry.expected_crc32) {
                require(CRC_GET_DIGEST(crc) == *entry.expected_crc32, Status::corrupt, L"归档文件 CRC-32 校验失败：" + entry.path.wstring());
                entry.source_hash_verified = true;
            }
            if (!FlushFileBuffers(file.get())) platform::io_failure(L"无法刷新归档输出");
            file.reset(); entry.sha256 = platform::sha256(output.full_path(entry.path));
            log::verified(entry);
        }
        folder_data.reset();
        progress::Scope finalizing(progress::Phase::finalizing);
        const auto report = platform::utf8(catalog_json(catalog, true));
        { auto file = output.create_file(L"_extract-report.json"); platform::write_all(file.get(), std::as_bytes(std::span(report)));
          if (!FlushFileBuffers(file.get())) platform::io_failure(L"无法保存归档报告"); }
        return output.commit();
    }
};

void ArchivePackage::Impl::read_zip(std::size_t archive_offset) {
    const auto b = bytes;
    catalog.format = L"ZIP"; catalog.format_version = L"ZIP/ZIP64";
    require(b.size() >= 22, Status::corrupt, L"ZIP 尾记录缺失。");
    std::optional<std::size_t> end;
    const auto lower = b.size() > 65557 ? b.size() - 65557 : 0;
    for (auto p = b.size() - 22;; --p) {
        if (u32(b, p) == 0x06054b50 && io::Reader(io::slice(b, p + 20, 2)).u16() == b.size() - p - 22) { end = p; break; }
        if (p == lower) break;
    }
    require(end.has_value(), Status::corrupt, L"ZIP 尾记录或注释长度无效。");
    io::Reader eocd(io::slice(b, *end + 4, 16));
    const auto disk_number = eocd.u16(), central_disk = eocd.u16();
    // MakeAppx 的 ZIP64 尾记录也把卷号写成占位值；实际卷号在 ZIP64
    // 记录中继续严格验证为零，不能因此接受真正的分卷。
    require((disk_number == 0 || disk_number == 0xffff) && (central_disk == 0 || central_disk == 0xffff),
            Status::unsupported, L"当前不支持 ZIP 分卷。");
    const auto disk_entries = eocd.u16(); std::uint64_t count = eocd.u16();
    std::uint64_t central_size = eocd.u32(), central_offset = eocd.u32(), central_end = *end;
    require(disk_entries == count, Status::unsupported, L"ZIP 跨卷文件数量不一致。");
    if (disk_number == 0xffff || central_disk == 0xffff || count == 0xffff || central_size == 0xffffffffU || central_offset == 0xffffffffU) {
        require(*end >= 20, Status::corrupt, L"ZIP64 定位记录缺失。");
        io::Reader locator(io::slice(b, *end - 20, 20));
        require(locator.u32() == 0x07064b50, Status::corrupt, L"ZIP64 定位签名错误。");
        require(locator.u32() == 0, Status::unsupported, L"当前不支持 ZIP64 分卷。");
        auto offset = locator.u64(); require(locator.u32() == 1, Status::unsupported, L"当前不支持 ZIP64 分卷。");
        if (!(offset <= b.size() && b.size() - offset >= 4 && u32(b, offset) == 0x06064b50)) {
            require(offset <= b.size() - archive_offset, Status::corrupt, L"ZIP64 尾偏移越界。"); offset += archive_offset;
        }
        io::Reader tail(io::slice(b, offset, 56));
        require(tail.u32() == 0x06064b50, Status::corrupt, L"ZIP64 尾签名错误。");
        const auto record_size = tail.u64();
        require(record_size >= 44 && record_size <= metadata_limit && offset + 12 + record_size == *end - 20,
            Status::corrupt, L"ZIP64 尾长度无效。");
        tail.skip(4); require(tail.u32() == 0 && tail.u32() == 0, Status::unsupported, L"当前不支持 ZIP64 分卷。");
        const auto on_disk = tail.u64(); count = tail.u64();
        require(on_disk == count, Status::unsupported, L"当前不支持 ZIP64 分卷。");
        central_size = tail.u64(); central_offset = tail.u64(); central_end = offset;
    }
    require(central_size <= metadata_limit, Status::limit_exceeded, L"ZIP 中央目录元数据超过内存预算。");
    require(count <= central_size / 46, Status::corrupt, L"ZIP 条目数量超出实际中央目录范围。");
    require(central_size <= central_end, Status::corrupt, L"ZIP 中央目录长度越界。");
    const auto central_start = central_end - central_size;
    require(central_offset <= central_start, Status::corrupt, L"ZIP 中央目录偏移无效。");
    const auto bias = central_start - central_offset;
    require(bias == 0 || bias == archive_offset, Status::unsupported, L"ZIP SFX 偏移布局尚未支持。");
    io::Reader central(io::slice(b, central_start, central_size));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    for (std::uint64_t i = 0; i < count; ++i) {
        require(central.u32() == 0x02014b50, Status::corrupt, L"ZIP 中央目录签名无效。");
        central.skip(4); const auto flags = central.u16(), method = central.u16(); central.skip(4);
        const auto crc = central.u32(); std::uint64_t packed = central.u32(), size = central.u32();
        const bool large_packed = packed == 0xffffffffU, large_size = size == 0xffffffffU;
        const auto name_size = central.u16(), extra_size = central.u16(), comment_size = central.u16();
        std::uint32_t disk = central.u16(); central.skip(2); const auto attr = central.u32();
        std::uint64_t local_offset = central.u32();
        const bool large_offset = local_offset == 0xffffffffU, large_disk = disk == 0xffff;
        require((flags & ~0x080eU) == 0, Status::unsupported, L"ZIP 加密、补丁或扩展标志尚未支持。");
        require(method == 0 || method == 8 || method == 12, Status::unsupported, L"ZIP 当前支持 stored、Deflate 和 bzip2。");
        const auto raw_name = central.take(name_size);
        auto name = decode_name(raw_name, flags & 0x0800 ? CP_UTF8 : 437);
        io::Reader extra(central.take(extra_size)); central.skip(comment_size);
        bool zip64 = false, unicode = false;
        while (extra.remaining()) {
            const auto id = extra.u16(), length = extra.u16(); io::Reader value(extra.take(length));
            if (id == 1) {
                require(!zip64, Status::corrupt, L"ZIP64 扩展字段重复。"); zip64 = true;
                if (large_size) size = value.u64(); if (large_packed) packed = value.u64();
                if (large_offset) local_offset = value.u64(); if (large_disk) disk = value.u32();
            } else if (id == 0x7075) {
                require(!unicode, Status::corrupt, L"ZIP Unicode 路径字段重复。"); unicode = true;
                const auto version = value.u8(); const auto name_crc = value.u32();
                if (version == 1 && name_crc == codecs::crc32(raw_name)) name = decode_name(value.take(value.remaining()), CP_UTF8);
            }
        }
        require(!(large_size || large_packed || large_offset || large_disk) || zip64, Status::corrupt, L"ZIP64 扩展字段缺失。");
        require(disk == 0, Status::unsupported, L"当前不支持 ZIP 分卷。"); attributes(attr);
        const bool directory = !name.empty() && (name.back() == L'/' || name.back() == L'\\' || (attr & FILE_ATTRIBUTE_DIRECTORY));
        const auto path = archive_path(name, directory);
        require(local_offset <= central_start && bias <= central_start - local_offset, Status::corrupt, L"ZIP 本地头偏移越界。");
        const auto local_start = local_offset + bias;
        io::Reader local(io::slice(b, local_start, central_start - local_start));
        require(local.u32() == 0x04034b50, Status::corrupt, L"ZIP 本地头签名错误。");
        local.skip(2); require(local.u16() == flags && local.u16() == method, Status::corrupt, L"ZIP 本地头与中央目录标志不一致。");
        local.skip(4); const auto local_crc = local.u32(); std::uint64_t local_packed = local.u32(), local_size = local.u32();
        const bool local_large_size = local_size == 0xffffffffU, local_large_packed = local_packed == 0xffffffffU;
        const auto local_name_size = local.u16(), local_extra_size = local.u16();
        const auto local_name = local.take(local_name_size);
        require(std::ranges::equal(local_name, raw_name), Status::corrupt, L"ZIP 本地文件名与中央目录不一致。");
        io::Reader local_extra(local.take(local_extra_size)); bool local_zip64 = false;
        while (local_extra.remaining()) {
            const auto id = local_extra.u16(), length = local_extra.u16(); io::Reader value(local_extra.take(length));
            if (id != 1) continue;
            require(!local_zip64, Status::corrupt, L"ZIP64 本地字段重复。"); local_zip64 = true;
            if (local_large_size) local_size = value.u64(); if (local_large_packed) local_packed = value.u64();
        }
        require(!(local_large_size || local_large_packed) || local_zip64, Status::corrupt, L"ZIP64 本地字段缺失。");
        if (!(flags & 8)) require(local_crc == crc && local_size == size && local_packed == packed, Status::corrupt, L"ZIP 本地大小或 CRC 不一致。");
        const auto data = local_start + local.position();
        require(packed <= central_start - data, Status::corrupt, L"ZIP 载荷越过中央目录。");
        auto data_end = data + packed;
        if (flags & 8) {
            io::Reader descriptor(io::slice(b, data_end, central_start - data_end));
            auto actual_crc = descriptor.u32(); if (actual_crc == 0x08074b50) actual_crc = descriptor.u32();
            const bool wide = large_packed || large_size || local_large_packed || local_large_size;
            const auto actual_packed = wide ? descriptor.u64() : descriptor.u32();
            const auto actual_size = wide ? descriptor.u64() : descriptor.u32();
            require(actual_crc == crc && actual_size == size && actual_packed == packed, Status::corrupt, L"ZIP 数据描述符不一致。");
            data_end += descriptor.position();
        }
        spans.emplace_back(local_start, data_end);
        if (directory) { require(size == 0, Status::corrupt, L"ZIP 目录条目含文件数据。"); continue; }
        Entry file; file.id = L"zip-" + std::to_wstring(i); file.path = file.original_path = path;
        file.source_expression = name; file.size = size; file.expected_crc32 = crc; add(std::move(file));
        zip.push_back({data, packed, method == 0 ? codecs::Compression::stored : (method == 8 ? codecs::Compression::raw_deflate : codecs::Compression::bzip2), data - local_start});
    }
    require(central.remaining() == 0, Status::corrupt, L"ZIP 中央目录含未知尾数据。");
    std::sort(spans.begin(), spans.end());
    for (std::size_t i = 1; i < spans.size(); ++i)
        require(spans[i].first >= spans[i - 1].second, Status::corrupt, L"ZIP 文件数据范围重叠。");
}

ArchivePackage::ArchivePackage(const fs::path& path) : impl_(std::make_unique<Impl>(path)) {}
ArchivePackage::ArchivePackage(const fs::path& path, io::Bytes bytes) : impl_(std::make_unique<Impl>(path, bytes)) {}
ArchivePackage::~ArchivePackage() = default;
const Catalog& ArchivePackage::catalog() const { return impl_->catalog; }
fs::path ArchivePackage::extract(const fs::path& parent) { return impl_->extract(parent); }
void ArchivePackage::read_member(std::size_t index, const std::function<void(io::Bytes)>& sink) const {
    require(!impl_->seven && index < impl_->zip.size(), Status::internal_error, L"ZIP 成员索引无效。");
    const auto& entry = impl_->catalog.files[index];
    const auto& payload = impl_->zip[index];
    codecs::Stream stream(payload.method, io::slice(impl_->bytes, payload.data, payload.packed));
    std::array<std::byte, 65536> buffer{};
    auto remaining = entry.size;
    UInt32 crc = CRC_INIT_VAL;
    while (remaining) {
        const auto n = static_cast<std::size_t>((std::min)(remaining, static_cast<std::uint64_t>(buffer.size())));
        const auto part = std::span(buffer).first(n);
        stream.read_exact(part); crc = CrcUpdate(crc, part.data(), part.size());
        sink(part); remaining -= n;
    }
    stream.finish();
    require(entry.expected_crc32 && CRC_GET_DIGEST(crc) == *entry.expected_crc32,
            Status::corrupt, L"ZIP 成员 CRC-32 校验失败：" + entry.original_path.wstring());
}
std::vector<std::byte> ArchivePackage::read_metadata(std::size_t index, std::size_t limit) const {
    require(index < impl_->catalog.files.size(), Status::internal_error, L"ZIP 元数据索引无效。");
    require(impl_->catalog.files[index].size <= limit, Status::limit_exceeded, L"更新包清单超过元数据预算。");
    std::vector<std::byte> result;
    result.reserve(static_cast<std::size_t>(impl_->catalog.files[index].size));
    read_member(index, [&](io::Bytes bytes) { result.insert(result.end(), bytes.begin(), bytes.end()); });
    return result;
}
ZipMember ArchivePackage::zip_member(std::size_t index) const {
    require(!impl_->seven && index < impl_->zip.size(), Status::internal_error, L"ZIP 成员索引无效。");
    const auto& member = impl_->zip[index];
    return {member.data, member.local_header_size, member.method, io::slice(impl_->bytes, member.data, member.packed)};
}
void ArchivePackage::set_block_verifier(std::function<void(std::size_t, std::uint64_t, io::Bytes)> verifier,
                                      const std::vector<std::wstring>& algorithms) {
    require(!impl_->seven && algorithms.size() == impl_->catalog.files.size(), Status::internal_error, L"块校验配置无效。");
    impl_->verifier = std::move(verifier);
    for (std::size_t i = 0; i < algorithms.size(); ++i) impl_->catalog.files[i].block_hash_algorithm = algorithms[i];
}
void ArchivePackage::set_layout(std::wstring format, std::wstring version,
                               const std::vector<fs::path>& paths, std::vector<std::wstring> notes,
                               bool complete, const std::vector<std::wstring>& conditions) {
    auto& catalog = impl_->catalog;
    require(paths.size() == catalog.files.size(), Status::internal_error, L"更新包目录映射数量不一致。");
    require(conditions.empty() || conditions.size() == paths.size(), Status::internal_error, L"包条件映射数量不一致。");
    for (std::size_t i = 0; i < paths.size(); ++i) {
        platform::validate_relative(paths[i]); catalog.files[i].path = paths[i];
        if (!conditions.empty()) catalog.files[i].conditions = conditions[i];
    }
    catalog.format = std::move(format); catalog.format_version = std::move(version);
    catalog.notes = std::move(notes);
    catalog.content_complete = complete;
    plan_paths(catalog);
}
}
