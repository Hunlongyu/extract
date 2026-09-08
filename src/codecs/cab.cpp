#include "codecs/cab.h"
#include "platform/log.h"
#include "core/progress.h"
#include "core/control.h"
#include <fdi.h>
#include <msi.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace extract::codecs {
namespace {
struct Volume {
    io::Bytes bytes;
    std::string name, previous, next;
    std::uint16_t set = 0, index = 0;
    std::vector<CabMember> members;
};
struct NameLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
std::string cab_string(io::Reader& r, std::size_t limit) {
    std::string value;
    for (;;) { const auto c = r.u8(); if (!c) return value;
        require(value.size() < limit, Status::corrupt, L"CAB 字符串过长。"); value += static_cast<char>(c); }
}
bool same_cabinet(const std::string& a, const std::string& b) {
    return !a.empty() && !b.empty() && platform::equal_name(cabinet_path(a), cabinet_path(b));
}
Volume read_volume(io::Bytes bytes) {
    Volume volume; volume.bytes = bytes;
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
    volume.set = r.u16(); volume.index = r.u16();
    require((flags & ~7u) == 0, Status::unsupported, L"未知 CAB 标志。");
    std::uint8_t folder_reserve = 0;
    if (flags & 4) {
        const auto header_reserve = r.u16(); folder_reserve = r.u8(); r.skip(1); r.skip(header_reserve);
    }
    if (flags & 1) { volume.previous = cab_string(r, 255); (void)cab_string(r, 255); (void)cabinet_path(volume.previous); }
    if (flags & 2) { volume.next = cab_string(r, 255); (void)cab_string(r, 255); (void)cabinet_path(volume.next); }
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
        require(folder < folders || (folders && folder >= 0xfffd), Status::corrupt, L"CAB 文件目录编号无效。");
        if (folder == 0xfffd || folder == 0xffff) require(flags & 1, Status::corrupt, L"CAB 续接文件缺少前卷声明。");
        if (folder == 0xfffe || folder == 0xffff) require(flags & 2, Status::corrupt, L"CAB 续接文件缺少后卷声明。");
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
        result.push_back({std::move(name), std::move(decoded), size, offset, folder});
    }
    for (auto offset : data_offsets)
        require(offset >= static_cast<std::uint64_t>(file_offset) + table.position(), Status::corrupt, L"CAB 数据与文件表重叠。");
    volume.members = std::move(result); return volume;
}
}
std::wstring cabinet_path(const std::string& name) {
    require(!name.empty() && name.size() <= 255, Status::unsafe_path, L"CAB 分卷名称无效。");
    const auto n = MultiByteToWideChar(CP_ACP, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    require(n > 0, Status::unsafe_path, L"CAB 分卷名称编码无效。");
    std::wstring path(n, L'\0');
    require(MultiByteToWideChar(CP_ACP, 0, name.data(), static_cast<int>(name.size()), path.data(), n) == n, Status::unsafe_path, L"CAB 分卷名称转换失败。");
    platform::validate_component(path); return path;
}
std::string cabinet_name(const std::wstring& name) {
    platform::validate_component(name);
    BOOL fallback = FALSE;
    const bool utf8 = GetACP() == CP_UTF8;
    const auto flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    const auto n = WideCharToMultiByte(CP_ACP, flags, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, utf8 ? nullptr : &fallback);
    require(n > 0 && n <= 255 && !fallback, Status::unsupported, L"CAB 分卷名称不能表示为系统 ANSI 编码。");
    std::string value(n, '\0');
    require(WideCharToMultiByte(CP_ACP, flags, name.data(), static_cast<int>(name.size()), value.data(), n, nullptr, utf8 ? nullptr : &fallback) == n && !fallback,
            Status::unsupported, L"CAB 分卷名称编码失败。"); return value;
}
std::vector<CabMember> cab_members(io::Bytes bytes) {
    auto volume = read_volume(bytes);
    require(volume.previous.empty() && volume.next.empty(), Status::unsupported, L"此容器尚未接入 CAB 跨卷来源。");
    return std::move(volume.members);
}

void verify_file(Entry& entry, const fs::path& path) {
    log::Scope step(L"file.verify", nullptr, &path);
    if (entry.msi_hash) {
        progress::Scope stage(progress::Phase::verifying, {}, entry.path.native());
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
    struct Reader { io::Bytes bytes; std::size_t position = 0; };
    std::map<INT_PTR, Reader> readers;
    INT_PTR next_reader = 2;
    platform::Handle destination;
    Entry* entry = nullptr;
    std::uint64_t written = 0;
    std::size_t next_member = 0;
    std::exception_ptr error;
    std::size_t allocated = 0;
    const std::vector<Volume>* volumes = nullptr;
    std::size_t root_volume = 0;
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
        io::Bytes bytes = current->bytes;
        if (current->volumes) {
            const auto& volumes = *current->volumes;
            const auto found = std::find_if(volumes.begin(), volumes.end(), [&](const auto& v) { return same_cabinet(v.name, name); });
            require(found != volumes.end(), Status::unsafe_path, L"CAB 解码请求了未经校验的分卷。"); bytes = found->bytes;
        } else require(std::strcmp(name, "payload.cab") == 0, Status::unsupported, L"CAB 解码不得打开其它文件。");
        const auto id = current->next_reader++; current->readers.emplace(id, Context::Reader{bytes, 0}); return id;
    } catch (...) { current->error = std::current_exception(); return -1; }
}
UINT DIAMONDAPI read(INT_PTR id, void* destination, UINT requested) noexcept {
    try {
        control::checkpoint(); auto& reader = current->readers.at(id);
        const auto count = (std::min)(static_cast<std::size_t>(requested), reader.bytes.size() - reader.position);
        if (count) std::memcpy(destination, reader.bytes.data() + reader.position, count);
        reader.position += count; return static_cast<UINT>(count);
    } catch (...) { current->error = std::current_exception(); return static_cast<UINT>(-1); }
}
long DIAMONDAPI seek(INT_PTR id, long distance, int origin) noexcept {
    try {
        auto& reader = current->readers.at(id); auto& position = reader.position;
        require(origin >= 0 && origin <= 2, Status::corrupt, L"CAB 读取定位无效。");
        const auto base = origin == SEEK_SET ? 0 : (origin == SEEK_CUR ? position : reader.bytes.size());
        const auto target = static_cast<std::int64_t>(base) + distance;
        require(target >= 0 && target <= (std::numeric_limits<long>::max)() &&
            static_cast<std::uint64_t>(target) <= reader.bytes.size(), Status::corrupt, L"CAB 读取越界。");
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
        else platform::write_payload(c.destination.get(), {bytes, count});
        c.written += count; return count;
    } catch (...) { current->error = std::current_exception(); return static_cast<UINT>(-1); }
}
INT_PTR DIAMONDAPI notify(FDINOTIFICATIONTYPE type, PFDINOTIFICATION n) noexcept {
    try {
        auto& c = *current;
        if (type == fdintNEXT_CABINET) {
            require(c.volumes && n->fdie == FDIERROR_NONE, Status::corrupt, L"CAB 续接失败：" + cabinet_path(n->psz1));
            const auto& volumes = *c.volumes;
            const auto found = std::find_if(volumes.begin(), volumes.end(), [&](const auto& v) { return same_cabinet(v.name, n->psz1); });
            require(found != volumes.end() && found->set == n->setID && found->index == n->iCabinet,
                    Status::corrupt, L"CAB 续接请求与已校验卷链不符。"); n->psz3[0] = '\0'; return 0;
        }
        if (type == fdintPARTIAL_FILE) {
            require(c.volumes && c.root_volume > 0, Status::corrupt, L"CAB 缺少文件起始分卷。"); return 0;
        }
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
            if (c.output) {
                progress::file(c.entry->path.native());
                c.destination = c.output->create_file(c.entry->path);
            }
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
            io::Output* output, std::vector<std::byte>* memory, const std::vector<Volume>* volumes = nullptr, std::size_t root = 0) {
    log::Scope step(L"cab.decode");
    require(!current && targets.size() == members.size(), Status::internal_error, L"CAB 解码参数无效。");
    Context context{bytes, members, targets, output, memory, {}, 2, platform::Handle{}, nullptr, 0, 0, {}, 0, volumes, root};
    current = &context;
    struct Reset { ~Reset() { current = nullptr; } } reset;
    ERF error{};
    const auto fdi = FDICreate(allocate, release, open, read, write, close, seek, cpuUNKNOWN, &error);
    require(fdi != nullptr, Status::limit_exceeded, L"无法创建 CAB 解码器。");
    struct Guard { HFDI fdi; ~Guard() { FDIDestroy(fdi); } } guard{fdi};
    std::string name = volumes ? (*volumes)[root].name : "payload.cab"; char directory[] = "";
    const auto copied = FDICopy(fdi, name.data(), directory, 0, notify, nullptr, &context);
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
struct CabinetSet::Impl {
    std::vector<Volume> volumes;
    std::vector<CabMember> members;
    std::vector<std::vector<CabMember>> starts;
    Impl(io::Bytes bytes, std::string name, const std::function<io::Bytes(const std::string&)>& resolver) {
        auto current_volume = read_volume(bytes); current_volume.name = std::move(name);
        // 单卷不依赖物理文件名，保留中文及 Unicode 路径支持。
        if (current_volume.previous.empty() && current_volume.next.empty()) current_volume.name = "payload.cab";
        std::map<unsigned, Volume> found;
        std::map<std::wstring, unsigned, NameLess> names;
        std::uint64_t metadata = 0;
        const auto add = [&](Volume volume) {
            auto amount = static_cast<std::uint64_t>(sizeof(Volume)) + volume.name.size() + volume.previous.size() + volume.next.size();
            for (const auto& m : volume.members) amount = checked_size_sum(amount, sizeof(CabMember) + m.name.size() + m.decoded_name.size() * sizeof(wchar_t));
            metadata = checked_size_sum(metadata, amount);
            require(metadata <= 64 * 1024 * 1024, Status::limit_exceeded, L"CAB 卷链文件表超过 64 MiB 元数据预算。");
            require(names.emplace(cabinet_path(volume.name), volume.index).second, Status::corrupt, L"CAB 分卷名称重复。");
            require(found.emplace(volume.index, std::move(volume)).second, Status::corrupt, L"CAB 卷序号重复。");
        };
        const auto selected = current_volume.index;
        add(std::move(current_volume));
        const auto load = [&](const std::string& filename) {
            const auto path = cabinet_path(filename);
            log::write(log::Level::info, L"cab.open_volume", path);
            try { auto v = read_volume(resolver(filename)); v.name = filename; return v; }
            catch (const Failure& e) { throw Failure(e.status, L"读取 CAB 分卷失败：" + path + L"；" + e.message, e.native_code); }
        };
        for (auto at = selected; !found.at(at).previous.empty();) {
            control::checkpoint(); const auto& v = found.at(at);
            require(at > 0, Status::corrupt, L"CAB 首卷仍引用前卷。"); auto previous = load(v.previous);
            require(previous.index < at && previous.set == v.set,
                    Status::corrupt, L"CAB 前卷编号、集合或链接不符。");
            at = previous.index; add(std::move(previous));
        }
        // CFHEADER 的前卷名称可指向跨卷文件的起始卷，并非总是相邻卷。
        // 先回到起始卷，再沿 next 顺序填齐所有中间卷。
        for (auto at = found.begin()->first; !found.at(at).next.empty();) {
            control::checkpoint(); const auto& v = found.at(at);
            if (!found.contains(at + 1)) {
                auto next = load(v.next);
                require(at < 65535 && next.index == at + 1 && next.set == v.set, Status::corrupt, L"CAB 后卷编号、集合或链接不符。");
                add(std::move(next));
            }
            const auto& next = found.at(at + 1);
            require(at < 65535 && next.index == at + 1 && next.set == v.set && same_cabinet(next.name, v.next),
                    Status::corrupt, L"CAB 后卷编号、集合或链接不符。");
            const auto previous = next.previous.empty() ? names.end() : names.find(cabinet_path(next.previous));
            require(previous != names.end() && previous->second <= at,
                    Status::corrupt, L"CAB 前卷链接不属于已校验的卷链。");
            ++at;
        }
        std::vector<CabMember> pending;
        auto expected_index = found.begin()->first;
        for (auto& [index, v] : found) {
            require(index == expected_index++, Status::corrupt, L"CAB 卷链不连续。");
            require(volumes.empty() || same_cabinet(volumes.back().next, v.name),
                    Status::corrupt, L"CAB 卷链存在未连接的分卷。");
            std::vector<CabMember> incoming, outgoing, local;
            for (const auto& m : v.members) {
                if (m.folder == 0xfffd || m.folder == 0xffff) incoming.push_back(m); else local.push_back(m);
                if (m.folder == 0xfffe || m.folder == 0xffff) outgoing.push_back(m);
            }
            require(incoming.size() == pending.size(), Status::corrupt, L"CAB 跨卷文件清单数量不一致。");
            for (std::size_t i = 0; i < incoming.size(); ++i)
                require(incoming[i].name == pending[i].name && incoming[i].size == pending[i].size && incoming[i].offset == pending[i].offset,
                        Status::corrupt, L"CAB 跨卷文件名称、大小或偏移不一致。");
            pending = std::move(outgoing); members.insert(members.end(), local.begin(), local.end());
            starts.push_back(std::move(local)); volumes.push_back(std::move(v));
        }
        require(pending.empty(), Status::corrupt, L"CAB 跨卷文件缺少结束卷。");
        require(volumes.back().next.empty(), Status::corrupt, L"CAB 卷链缺少结束卷。");
    }
};
CabinetSet::CabinetSet(io::Bytes first, std::string name, const std::function<io::Bytes(const std::string&)>& resolver)
    : impl_(std::make_unique<Impl>(first, std::move(name), resolver)) {}
CabinetSet::~CabinetSet() = default;
const std::vector<CabMember>& CabinetSet::members() const { return impl_->members; }
std::size_t CabinetSet::volume_count() const { return impl_->volumes.size(); }
void CabinetSet::extract(std::span<Entry*> targets, io::Output& output) {
    require(targets.size() == impl_->members.size(), Status::internal_error, L"CAB 卷链输出数量不符。");
    std::size_t first = 0;
    for (std::size_t i = 0; i < impl_->volumes.size(); ++i) {
        const auto count = impl_->starts[i].size();
        if (count) decode(impl_->volumes[i].bytes, impl_->starts[i], targets.subspan(first, count), &output, nullptr, &impl_->volumes, i);
        first += count;
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
