#include "codecs/cab.h"
#include "platform/log.h"
#include <fdi.h>
#include <msi.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>

namespace extract::codecs {
std::vector<CabMember> cab_members(io::Bytes bytes) {
    require(bytes.size() <= static_cast<std::uint64_t>((std::numeric_limits<long>::max)()),
        Status::unsupported, L"此 CAB 超出 Windows FDI 的有符号 32 位定位范围。");
    io::Reader r(bytes);
    require(r.u32() == 0x4643534d, Status::corrupt, L"CAB 标识损坏。");
    require(r.u32() == 0, Status::corrupt, L"CAB 保留字段无效。");
    require(r.u32() == bytes.size(), Status::corrupt, L"CAB 声明长度与实际数据不符。");
    require(r.u32() == 0, Status::corrupt, L"CAB 保留字段无效。");
    const auto file_offset = r.u32();
    require(r.u32() == 0, Status::corrupt, L"CAB 保留字段无效。");
    const auto minor = r.u8(), major = r.u8();
    require(major == 1 && minor == 3, Status::unsupported, L"不支持此 CAB 结构版本。");
    const auto folders = r.u16(), files = r.u16(), flags = r.u16();
    r.skip(4); // setID 和集合编号；无跨卷标志时可独立提取。
    require((flags & ~7u) == 0, Status::unsupported, L"未知 CAB 标志。");
    require((flags & 3) == 0, Status::unsupported, L"暂不支持跨卷 CAB；需要前后卷续接。");
    std::uint8_t folder_reserve = 0;
    if (flags & 4) {
        const auto header_reserve = r.u16(); folder_reserve = r.u8(); r.skip(1); r.skip(header_reserve);
    }
    std::vector<std::uint32_t> data_offsets;
    for (unsigned i = 0; i < folders; ++i) {
        const auto offset = r.u32(); const auto blocks = r.u16(); r.skip(2 + folder_reserve);
        require(offset <= bytes.size() && (!blocks || offset < bytes.size()), Status::corrupt, L"CAB 压缩块偏移越界。");
        data_offsets.push_back(offset);
    }
    require(file_offset >= r.position() && file_offset <= bytes.size(), Status::corrupt, L"CAB 文件表偏移无效。");
    io::Reader table(bytes.subspan(file_offset));
    std::vector<CabMember> result;
    std::uint64_t total = 0;
    std::size_t name_bytes = 0;
    for (unsigned i = 0; i < files; ++i) {
        const auto size = table.u32(); const auto offset = table.u32(); const auto folder = table.u16();
        table.skip(4); const auto attributes = table.u16();
        require(folder < folders, Status::unsupported, L"CAB 文件依赖其它卷或目录编号无效。");
        require(static_cast<std::uint64_t>(offset) + size <= (std::numeric_limits<std::uint32_t>::max)(),
                Status::unsupported, L"CAB 文件超出格式的 32 位偏移范围。");
        total = checked_size_sum(total, size);
        std::string name;
        for (;;) {
            const auto c = table.u8(); if (!c) break;
            require(name.size() < 32767, Status::limit_exceeded, L"CAB 文件名过长。"); name += static_cast<char>(c);
        }
        require(!name.empty(), Status::corrupt, L"CAB 文件名为空。");
        require(name.size() <= 8 * 1024 * 1024 - name_bytes, Status::limit_exceeded, L"CAB 文件名元数据超过 8 MiB。");
        name_bytes += name.size();
        const UINT encoding = (attributes & 0x80) ? CP_UTF8 : CP_ACP;
        const DWORD options = encoding == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
        const auto length = MultiByteToWideChar(encoding, options, name.data(), static_cast<int>(name.size()), nullptr, 0);
        require(length > 0, Status::corrupt, L"CAB 文件名编码无效。");
        std::wstring decoded(static_cast<std::size_t>(length), L'\0');
        require(MultiByteToWideChar(encoding, options, name.data(), static_cast<int>(name.size()), decoded.data(), length) == length,
                Status::corrupt, L"CAB 文件名解码失败。");
        result.push_back({std::move(name), std::move(decoded), size});
    }
    for (auto offset : data_offsets)
        require(offset >= static_cast<std::uint64_t>(file_offset) + table.position(), Status::corrupt, L"CAB 数据与文件表重叠。");
    return result;
}

void verify_file(Entry& entry, const fs::path& path) {
    log::Scope step(L"file.verify", nullptr, &path);
    if (entry.msi_hash) {
        MSIFILEHASHINFO info{}; info.dwFileHashInfoSize = sizeof(info);
        const auto code = MsiGetFileHashW(platform::extended_path(path).c_str(), 0, &info);
        if (code != ERROR_SUCCESS) platform::io_failure(L"MSI 文件哈希读取失败", code);
        require(std::equal(entry.msi_hash->begin(), entry.msi_hash->end(), info.dwData), Status::corrupt, L"文件内容与 MsiFileHash 不符。");
    }
    entry.sha256 = platform::sha256(path);
    std::wstring actual, expected;
    if (!entry.expected_sha1.empty()) { actual = platform::sha1(path); expected = entry.expected_sha1; }
    if (!entry.expected_sha256.empty()) { actual = entry.sha256; expected = entry.expected_sha256; }
    if (!entry.expected_sha512.empty()) { actual = platform::sha512(path); expected = entry.expected_sha512; }
    if (!expected.empty()) {
        require(platform::equal_name(actual, expected), Status::corrupt, L"载荷内容与包内哈希不符：" + entry.original_path.wstring());
        entry.source_hash_verified = true;
    }
    log::verified(entry);
}

namespace {
struct Context {
    io::Bytes bytes;
    const std::vector<CabMember>& members;
    std::span<Entry*> targets;
    io::Output* output;
    std::vector<std::byte>* memory;
    std::map<INT_PTR, std::size_t> readers;
    INT_PTR next_reader = 2;
    platform::Handle destination;
    Entry* entry = nullptr;
    std::uint64_t written = 0;
    std::size_t next_member = 0;
    std::exception_ptr error;
    std::size_t allocated = 0;
};
thread_local Context* current = nullptr;
void* DIAMONDAPI allocate(ULONG count) noexcept {
    constexpr std::size_t limit = 64 * 1024 * 1024;
    if (!current || count > limit - current->allocated) return nullptr;
    auto* p = static_cast<std::max_align_t*>(std::malloc(sizeof(std::max_align_t) + count));
    if (!p) return nullptr;
    std::memcpy(p, &count, sizeof(count)); current->allocated += count; return p + 1;
}
void DIAMONDAPI release(void* value) noexcept {
    if (!value) return;
    auto* p = static_cast<std::max_align_t*>(value) - 1;
    ULONG count = 0; std::memcpy(&count, p, sizeof(count));
    if (current) current->allocated -= count;
    std::free(p);
}
INT_PTR DIAMONDAPI open(char* name, int, int) noexcept {
    try {
        require(std::strcmp(name, "payload.cab") == 0, Status::unsupported, L"CAB 解码不得打开其它文件。");
        const auto id = current->next_reader++; current->readers.emplace(id, 0); return id;
    } catch (...) { current->error = std::current_exception(); return -1; }
}
UINT DIAMONDAPI read(INT_PTR id, void* destination, UINT requested) noexcept {
    try {
        auto& position = current->readers.at(id);
        const auto count = (std::min)(static_cast<std::size_t>(requested), current->bytes.size() - position);
        if (count) std::memcpy(destination, current->bytes.data() + position, count);
        position += count; return static_cast<UINT>(count);
    } catch (...) { current->error = std::current_exception(); return static_cast<UINT>(-1); }
}
long DIAMONDAPI seek(INT_PTR id, long distance, int origin) noexcept {
    try {
        auto& position = current->readers.at(id);
        require(origin >= 0 && origin <= 2, Status::corrupt, L"CAB 读取定位无效。");
        const auto base = origin == SEEK_SET ? 0 : (origin == SEEK_CUR ? position : current->bytes.size());
        const auto target = static_cast<std::int64_t>(base) + distance;
        require(target >= 0 && target <= (std::numeric_limits<long>::max)() &&
            static_cast<std::uint64_t>(target) <= current->bytes.size(), Status::corrupt, L"CAB 读取越界。");
        position = static_cast<std::size_t>(target); return static_cast<long>(position);
    } catch (...) { current->error = std::current_exception(); return -1; }
}
int DIAMONDAPI close(INT_PTR id) noexcept {
    if (id == 1) current->destination.reset(); else current->readers.erase(id);
    return 0;
}
UINT DIAMONDAPI write(INT_PTR id, void* data, UINT count) noexcept {
    try {
        auto& c = *current;
        require(id == 1 && c.entry, Status::corrupt, L"CAB 输出句柄无效。");
        require(count <= c.entry->size - c.written, Status::corrupt, L"CAB 输出超过声明大小。");
        const auto* bytes = static_cast<const std::byte*>(data);
        if (c.memory) c.memory->insert(c.memory->end(), bytes, bytes + count);
        else platform::write_all(c.destination.get(), {bytes, count});
        c.written += count; return count;
    } catch (...) { current->error = std::current_exception(); return static_cast<UINT>(-1); }
}
INT_PTR DIAMONDAPI notify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION n) noexcept {
    try {
        auto& c = *current;
        if (type == fdintNEXT_CABINET || type == fdintPARTIAL_FILE) throw Failure(Status::unsupported, L"暂不支持跨卷 CAB。");
        if (type == fdintCOPY_FILE) {
            require(!c.entry && c.next_member < c.members.size(), Status::corrupt, L"CAB 成员顺序无效。");
            const auto index = c.next_member++;
            const auto& member = c.members[index];
            require(member.name == n->psz1 && n->cb >= 0 && static_cast<std::uint64_t>(n->cb) == member.size,
                    Status::corrupt, L"CAB 解码清单与文件表不符。");
            if (!c.targets[index]) return 0;
            c.entry = c.targets[index]; c.written = 0;
            log::detail(log::Level::info, L"cab.file_begin", [&] { return c.entry->path.wstring() + L"; member=" + member.decoded_name; });
            require(c.entry->size == member.size, Status::corrupt, L"CAB 成员与安装包文件大小不符。");
            if (c.output) c.destination = c.output->create_file(c.entry->path);
            return 1;
        }
        if (type == fdintCLOSE_FILE_INFO) {
            require(n->hf == 1 && c.entry && c.written == c.entry->size, Status::corrupt, L"CAB 文件未完整写入。");
            if (c.output) {
                if (!FlushFileBuffers(c.destination.get())) platform::io_failure(L"刷新输出文件失败");
                c.destination.reset(); verify_file(*c.entry, c.output->full_path(c.entry->path));
            }
            c.entry = nullptr; return TRUE; // 忽略 CAB 的运行标志。
        }
        return 0;
    } catch (...) { current->destination.reset(); current->error = std::current_exception(); return -1; }
}
void decode(io::Bytes bytes, const std::vector<CabMember>& members, std::span<Entry*> targets,
            io::Output* output, std::vector<std::byte>* memory) {
    log::Scope step(L"cab.decode");
    require(!current && targets.size() == members.size(), Status::internal_error, L"CAB 解码参数无效。");
    Context context{bytes, members, targets, output, memory, {}, 2, platform::Handle{}, nullptr, 0, 0, {}, 0};
    current = &context;
    struct Reset { ~Reset() { current = nullptr; } } reset;
    ERF error{};
    const auto fdi = FDICreate(allocate, release, open, read, write, close, seek, cpuUNKNOWN, &error);
    require(fdi != nullptr, Status::limit_exceeded, L"无法创建 CAB 解码器。");
    struct Guard { HFDI fdi; ~Guard() { FDIDestroy(fdi); } } guard{fdi};
    char name[] = "payload.cab", directory[] = "";
    const auto copied = FDICopy(fdi, name, directory, 0, notify, nullptr, &context);
    if (!copied || context.error) log::detail(log::Level::error, L"cab.decode_failed", [&] {
        return L"FDI=" + std::to_wstring(error.erfOper) + L"; file=" + (context.entry ? context.entry->path.wstring() : L"<metadata>");
    });
    if (context.error) std::rethrow_exception(context.error);
    const auto status = error.erfOper == FDIERROR_ALLOC_FAIL ? Status::limit_exceeded :
        (error.erfOper == FDIERROR_BAD_COMPR_TYPE ? Status::unsupported : Status::corrupt);
    require(copied != FALSE, status, L"CAB 解码失败（FDI " + std::to_wstring(error.erfOper) + L"）。");
    require(!context.entry && context.next_member == members.size(), Status::corrupt, L"CAB 未提供完整清单。");
}
}
void extract_cab(io::Bytes bytes, std::span<Entry*> targets, io::Output& output) {
    auto members = cab_members(bytes); decode(bytes, members, targets, &output, nullptr);
}
std::vector<std::byte> cab_member_bytes(io::Bytes bytes, std::size_t index, std::size_t limit) {
    auto members = cab_members(bytes);
    require(index < members.size(), Status::corrupt, L"CAB 元数据成员不存在。");
    require(members[index].size <= limit, Status::limit_exceeded, L"CAB 元数据成员超过内存预算。");
    Entry entry; entry.size = members[index].size;
    std::vector<Entry*> targets(members.size(), nullptr); targets[index] = &entry;
    std::vector<std::byte> result; result.reserve(static_cast<std::size_t>(entry.size));
    decode(bytes, members, targets, nullptr, &result); return result;
}
}
