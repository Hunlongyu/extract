#include "formats/nsis.h"
#include "platform/log.h"
#include "codecs/stream.h"
#include "io/output.h"
#include <algorithm>
#include <map>
#include <set>
#include <deque>

// 自行实现的磁盘解析器。布局依据 NSIS v312 的 fileform.h、fileform.cpp、
// fileform.c、exec.c、util.c；这里只读文件指令，不运行 NSIS 字节码。
namespace extract::formats {
namespace {
using codecs::Compression;
constexpr std::size_t metadata_limit = 64 * 1024 * 1024;
constexpr std::size_t instruction_limit = 100000;
std::uint32_t u32(io::Bytes bytes, std::uint64_t offset) { return io::Reader(io::slice(bytes, offset, 4)).u32(); }

struct Locator { std::uint64_t offset; bool wide_offsets; };
std::optional<Locator> locate(io::Bytes bytes) {
    if (bytes.size() < 64) return {};
    const auto pe = u32(bytes, 60);
    io::Reader coff(io::slice(bytes, pe, 24));
    require(coff.u32() == 0x4550, Status::corrupt, L"PE 签名损坏。");
    coff.skip(2); const auto count = coff.u16(); coff.skip(12); const auto optional_size = coff.u16();
    require(count > 0 && count <= 96, Status::corrupt, L"PE 节数量无效。");
    const auto optional_offset = static_cast<std::uint64_t>(pe) + 24;
    io::Reader optional(io::slice(bytes, optional_offset, optional_size));
    const auto magic = optional.u16();
    require(magic == 0x10b || magic == 0x20b, Status::unsupported, L"不支持此 PE 可选头格式。");
    const auto table_start = optional_offset + optional_size;
    std::uint64_t end = table_start + static_cast<std::uint64_t>(count) * 40;
    io::Reader table(io::slice(bytes, table_start, static_cast<std::uint64_t>(count) * 40));
    for (unsigned i = 0; i < count; ++i) {
        table.skip(16); const auto size = table.u32(), offset = table.u32(); table.skip(16);
        (void)io::slice(bytes, offset, size);
        if (size) end = (std::max)(end, static_cast<std::uint64_t>(offset) + size);
    }
    // 只查 PE 外层的对齐附加区，避免将压缩数据中的内层安装器当成外壳。
    if (end > 16 * 1024 * 1024) return {};
    const auto stop = (std::min)(static_cast<std::uint64_t>(bytes.size()), end + 65536);
    for (auto pos = (end + 511) & ~511ULL; pos + 28 <= stop; pos += 512) {
        if (u32(bytes, pos + 4) == 0xdeadbeef && u32(bytes, pos + 8) == 0x6c6c754e &&
            u32(bytes, pos + 12) == 0x74666f73 && u32(bytes, pos + 16) == 0x74736e49)
            return Locator{pos, magic == 0x20b};
    }
    return {};
}

std::vector<Compression> candidates(io::Bytes bytes) {
    std::vector<Compression> result;
    if (bytes.size() >= 5 && std::to_integer<unsigned>(bytes[0]) < 225) {
        const auto dictionary = u32(bytes, 1);
        if (dictionary >= 4096 && dictionary <= 256U * 1024 * 1024)
            result.push_back(Compression::lzma1);
    }
    if (!bytes.empty() && (bytes[0] == std::byte{0x31} || bytes[0] == std::byte{0x17}))
        result.push_back(Compression::nsis_bzip2);
    result.push_back(Compression::raw_deflate);
    return result;
}

// Solid 数据展开到受目录锁保护的临时文件，映射只占虚拟地址，不放进大 vector。
// --list 同样需要读取文件块长度；此缓存析构时按本任务创建清单删除。
class SolidData {
public:
    SolidData(Compression method, io::Bytes input) : output_(temporary_parent(), L"nsis-cache") {
        log::Scope step(L"nsis.decode_solid_cache");
        file_ = output_.create_file(L"stream.bin", true);
        codecs::Stream stream(method, input);
        std::array<std::byte, 65536> buffer{};
        for (;;) {
            const auto size = stream.read(buffer);
            if (!size) break;
            require(size <= max_output_bytes + metadata_limit + 4 - size_, Status::limit_exceeded, L"NSIS Solid 数据展开超过上限。");
            platform::write_all(file_.get(), std::span(buffer).first(size)); size_ += size;
        }
        require(size_ > 0, Status::corrupt, L"NSIS Solid 数据为空。");
        // 保留 CREATE_NEW 得到的原始句柄，始终拒绝其它写入/删除打开；只建立只读映射。
        mapping_ = platform::Handle(CreateFileMappingW(file_.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
        if (!mapping_) platform::io_failure(L"无法映射 NSIS Solid 缓存");
        view_ = static_cast<const std::byte*>(MapViewOfFile(mapping_.get(), FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(size_)));
        if (!view_) platform::io_failure(L"无法读取 NSIS Solid 缓存映射");
    }
    ~SolidData() { if (view_) UnmapViewOfFile(view_); }
    io::Bytes bytes() const { return {view_, static_cast<std::size_t>(size_)}; }
private:
    static fs::path temporary_parent() {
        std::wstring path(32768, L'\0');
        const auto size = GetTempPathW(static_cast<DWORD>(path.size()), path.data());
        if (size == 0 || size >= path.size()) platform::io_failure(L"无法获取 NSIS 缓存目录");
        path.resize(size); return fs::path(path).lexically_normal();
    }
    io::Output output_;
    platform::Handle file_, mapping_;
    const std::byte* view_ = nullptr;
    std::uint64_t size_ = 0;
};

struct Value {
    std::wstring text;
    bool known = true;
    bool rooted = false;
};
Value unknown(std::wstring name) { return {std::move(name), false, false}; }
void append(Value& target, const Value& part) {
    if (target.text.empty()) target.rooted = part.rooted;
    else if (part.rooted) target.known = false;
    target.text += part.text; target.known = target.known && part.known;
    require(target.text.size() <= 16000, Status::limit_exceeded, L"NSIS 字符串展开过长。");
}
struct State {
    Value out = unknown(L"$OUTDIR");
    std::map<unsigned, Value> variables;
};
struct Block { std::uint64_t offset; std::uint32_t count; };
struct Instruction { std::uint32_t op; std::array<std::int32_t, 6> p; };
struct Payload { io::Bytes bytes; Compression method; std::uint64_t size = 0; };
}

bool nsis_probe(io::Bytes input) { return locate(input).has_value(); }
bool nsis_uninstaller(io::Bytes input) {
    const auto found = locate(input);
    return found && (u32(input, found->offset) & 1) != 0;
}

struct NsisPackage::Impl {
    io::Input input;
    Catalog catalog;
    std::unique_ptr<SolidData> solid_data;
    std::vector<std::byte> header;
    std::array<Block, 8> blocks{};
    std::vector<Instruction> instructions;
    std::wstring strings;
    std::vector<std::vector<std::int32_t>> languages;
    std::map<std::uint64_t, Payload> payloads;
    std::vector<std::uint64_t> file_offsets;
    bool solid = false;
    std::optional<Compression> method;
    mutable std::size_t string_budget = 64 * 1024 * 1024;

    explicit Impl(const fs::path& path) : input(path) {
        catalog.input = input.path(); catalog.format = L"NSIS";
        const auto found = locate(input.bytes());
        require(found.has_value(), Status::unsupported, L"未发现 NSIS 安装包头。");
        const auto pos = found->offset;
        const auto flags = u32(input.bytes(), pos), length = u32(input.bytes(), pos + 20), all = u32(input.bytes(), pos + 24);
        require((flags & ~15U) == 0, Status::unsupported, L"不支持此 NSIS 标志组合。");
        require((flags & 1) == 0, Status::unsupported, L"当前不提取 NSIS 卸载程序。");
        require(length >= 300 && length <= metadata_limit, Status::limit_exceeded, L"NSIS 元数据长度超出支持范围。");
        const bool crc = (flags & 4) == 0;
        require(all >= 32 + (crc ? 4U : 0U), Status::corrupt, L"NSIS 声明长度无效。");
        const auto archive = io::slice(input.bytes(), pos, all);
        if (crc) {
            const auto expected = u32(archive, all - 4);
            require(pos >= 512 && codecs::crc32(io::slice(input.bytes(), 512, pos + all - 4 - 512)) == expected,
                Status::corrupt, L"NSIS 包内 CRC32 校验失败。");
        }
        catalog.package_crc_present = crc; catalog.package_crc_verified = crc;
        catalog.notes.push_back(crc ? L"NSIS 包内 CRC32 已验证；文件 SHA-256 为提取后计算，包内没有逐文件摘要。" :
            L"此 NSIS 包关闭了 CRC32；文件 SHA-256 仅为提取后计算。");
        auto compressed = archive.subspan(28, archive.size() - 28 - (crc ? 4 : 0));
        const auto first = u32(compressed, 0);
        std::size_t data_start = 0;
        if (first == length) {
            const auto bytes = io::slice(compressed, 4, length);
            header.assign(bytes.begin(), bytes.end()); data_start = 4 + length;
        } else {
            bool decoded = false;
            if ((first & 0x80000000U) && (first & 0x7fffffffU) <= compressed.size() - 4) {
                const auto packed = compressed.subspan(4, first & 0x7fffffffU);
                for (auto candidate : candidates(packed)) {
                    try {
                        auto bytes = codecs::decode_bounded(candidate, packed, length);
                        if (bytes.size() != length || u32(bytes, 4) != (found->wide_offsets ? 332U : 300U)) continue;
                        header = std::move(bytes); method = candidate; data_start = 4 + packed.size(); decoded = true; break;
                    } catch (const Failure& failure) {
                        if (failure.status != Status::corrupt && failure.status != Status::limit_exceeded) throw;
                    }
                }
            }
            if (!decoded) {
                for (auto candidate : candidates(compressed)) {
                    try {
                        codecs::Stream stream(candidate, compressed);
                        std::array<std::byte, 8> prefix{}; stream.read_exact(prefix);
                        if (u32(prefix, 0) != length) continue;
                        header.resize(length);
                        std::copy(prefix.begin() + 4, prefix.end(), header.begin());
                        stream.read_exact(std::span(header).subspan(4));
                        if (u32(header, 4) != (found->wide_offsets ? 332U : 300U)) continue;
                        method = candidate; solid = true; decoded = true; break;
                    } catch (const Failure& failure) {
                        if (failure.status != Status::corrupt && failure.status != Status::limit_exceeded) throw;
                    }
                }
            }
            require(decoded, Status::corrupt, L"NSIS 压缩头损坏或压缩变体不受支持。");
        }
        parse_header(found->wide_offsets);
        if (solid) {
            solid_data = std::make_unique<SolidData>(*method, compressed);
            compressed = solid_data->bytes(); data_start = 4 + length;
        }
        const auto data = io::slice(compressed, data_start, compressed.size() - data_start);
        for (std::uint64_t offset = 0; offset < data.size();) {
            require(payloads.size() < max_entries * 4, Status::limit_exceeded, L"NSIS 数据块过多。");
            const auto packed = u32(data, offset), size = packed & 0x7fffffffU;
            require(!solid || (packed & 0x80000000U) == 0, Status::corrupt, L"NSIS Solid 内部块长度无效。");
            const auto bytes = io::slice(data, offset + 4, size);
            payloads.emplace(offset, Payload{bytes, (packed & 0x80000000U) ? Compression::raw_deflate : Compression::stored, 0});
            offset += 4ULL + size;
        }
        plan_files();
        std::set<std::uint64_t> measured;
        for (std::size_t i = 0; i < catalog.files.size(); ++i) {
            auto& payload = payloads.at(file_offsets[i]);
            if (measured.insert(file_offsets[i]).second) measure(payload);
            catalog.files[i].size = payload.size;
            require(payload.size <= max_output_bytes - catalog.total_size, Status::limit_exceeded, L"NSIS 文件总输出超过 8 GiB。");
            catalog.total_size += payload.size;
        }
        require(!catalog.files.empty(), Status::unsupported, L"NSIS 包没有可提取的内嵌文件指令。");
        plan_paths(catalog);
        catalog.notes.push_back(L"静态提取所有 File 指令；不执行条件、安装操作或插件，不生成卸载器或脚本写入的文件。");
        catalog.notes.push_back(L"files 记录本层文件；提取时会继续展开已识别的内嵌安装包，结果记录在 nestedPackages。--list 只列本层。");
        catalog.notes.push_back(solid ? L"Solid 压缩；使用有大小上限的临时磁盘缓存。" : L"逐块压缩或不压缩。");
    }

    void parse_header(bool wide) {
        const std::size_t root_size = wide ? 332 : 300;
        io::Reader reader(header); reader.skip(4);
        for (auto& block : blocks) { block.offset = wide ? reader.u64() : reader.u32(); block.count = reader.u32(); }
        require(blocks[0].offset == root_size && blocks[1].offset >= blocks[0].offset &&
            blocks[2].offset >= blocks[1].offset && blocks[3].offset >= blocks[2].offset &&
            blocks[4].offset > blocks[3].offset && blocks[5].offset >= blocks[4].offset && blocks[5].offset <= header.size(),
            Status::unsupported, L"NSIS 块布局不属于受支持的 Unicode 3.x 格式。");
        require(blocks[2].count > 0 && blocks[2].count <= instruction_limit && blocks[1].count <= max_entries,
            Status::limit_exceeded, L"NSIS 指令或区段数量超出上限。");
        require(blocks[3].offset - blocks[2].offset == static_cast<std::uint64_t>(blocks[2].count) * 28,
            Status::corrupt, L"NSIS 指令表长度不一致。");
        if (blocks[1].count) {
            const auto section_size = (blocks[2].offset - blocks[1].offset) / blocks[1].count;
            require((section_size == 2072 || section_size == 16408) &&
                section_size * blocks[1].count == blocks[2].offset - blocks[1].offset,
                Status::unsupported, L"当前仅支持 NSIS Unicode 1024/8192 字符构建。");
        }
        const auto string_bytes = io::slice(header, blocks[3].offset, blocks[4].offset - blocks[3].offset);
        require(string_bytes.size() % 2 == 0 && string_bytes.size() >= 2, Status::corrupt, L"NSIS 字符串表长度无效。");
        io::Reader text_reader(string_bytes);
        while (text_reader.remaining()) strings += static_cast<wchar_t>(text_reader.u16());
        require(strings.front() == 0 && strings.back() == 0, Status::corrupt, L"NSIS 字符串表未终止。");
        const std::wstring marker = L"Nullsoft Install System v3.";
        const auto version = strings.find(marker);
        if (version == strings.npos) {
            // BrandingText 可合法替换默认版本文本；结构校验不能依赖 UI 品牌字符串。
            catalog.format_version = L"3.x Unicode (custom branding)";
            catalog.notes.push_back(L"默认 NSIS 版本文字已被替换；按 Unicode 块/指令布局识别，不猜测编译器补丁版本。");
        } else {
            const auto version_start = version + marker.size() - 2;
            const auto version_end = strings.find(L'\0', version_start);
            require(version_end - version_start <= 24, Status::corrupt, L"NSIS 版本字符串无效。");
            catalog.format_version = strings.substr(version_start, version_end - version_start);
        }
        const auto language_size = u32(header, 4 + (wide ? 12 : 8) * 8 + 32);
        require(blocks[4].count > 0 && blocks[4].count <= 256 && language_size >= 10 && (language_size - 10) % 4 == 0 &&
            static_cast<std::uint64_t>(language_size) * blocks[4].count == blocks[5].offset - blocks[4].offset,
            Status::corrupt, L"NSIS 语言表长度不一致。");
        for (std::uint32_t n = 0; n < blocks[4].count; ++n) {
            io::Reader language(io::slice(header, blocks[4].offset + static_cast<std::uint64_t>(n) * language_size, language_size));
            language.skip(10); std::vector<std::int32_t> values;
            while (language.remaining()) values.push_back(static_cast<std::int32_t>(language.u32()));
            languages.push_back(std::move(values));
        }
        io::Reader code(io::slice(header, blocks[2].offset, static_cast<std::uint64_t>(blocks[2].count) * 28));
        for (std::uint32_t n = 0; n < blocks[2].count; ++n) {
            Instruction instruction{}; instruction.op = code.u32();
            for (auto& p : instruction.p) p = static_cast<std::int32_t>(code.u32());
            require(instruction.op > 0 && instruction.op <= 73, Status::unsupported, L"NSIS 包包含未知指令编号。");
            instructions.push_back(instruction);
        }
    }

    Value string(std::int32_t index, const State& state, unsigned depth = 0) const {
        require(string_budget > 0, Status::limit_exceeded, L"NSIS 字符串解析工作量超过上限。");
        --string_budget;
        require(depth < 16, Status::corrupt, L"NSIS 字符串引用循环或嵌套过深。");
        if (index < 0) {
            const auto entry = static_cast<std::uint64_t>(-static_cast<std::int64_t>(index) - 1);
            std::optional<Value> result;
            for (const auto& language : languages) {
                require(entry < language.size(), Status::corrupt, L"NSIS 语言字符串索引越界。");
                const auto value = string(language[static_cast<std::size_t>(entry)], state, depth + 1);
                if (!result) result = value;
                else if (result->text != value.text || result->rooted != value.rooted || result->known != value.known)
                    return unknown(L"$LANG" + std::to_wstring(entry));
            }
            return *result;
        }
        require(static_cast<std::size_t>(index) < strings.size(), Status::corrupt, L"NSIS 字符串索引越界。");
        Value result;
        for (std::size_t i = static_cast<std::size_t>(index);; ++i) {
            require(string_budget > 0, Status::limit_exceeded, L"NSIS 字符串解析工作量超过上限。");
            --string_budget;
            require(i < strings.size(), Status::corrupt, L"NSIS 字符串未终止。");
            const auto c = strings[i];
            if (!c) break;
            if (c > 4) { append(result, Value{std::wstring(1, c)}); continue; }
            require(++i < strings.size(), Status::corrupt, L"NSIS 编码字符串截断。");
            const auto value = static_cast<unsigned>(strings[i]);
            if (c == 4) { require(value != 0, Status::corrupt, L"NSIS 字符转义无效。"); append(result, Value{std::wstring(1, static_cast<wchar_t>(value))}); continue; }
            const auto decoded = ((value >> 8) & 0x7f) * 128 + (value & 0x7f);
            if (c == 1) append(result, string(-static_cast<std::int32_t>(decoded) - 1, state, depth + 1));
            else if (c == 2) append(result, Value{L"$SHELL_" + std::to_wstring(value), true, true});
            else if (decoded == 22) append(result, state.out);
            // 这些是逻辑目录根；无需计算安装时的具体盘符或插件临时目录名。
            else if (decoded == 21) append(result, Value{L"$INSTDIR", true, true});
            else if (decoded == 23) append(result, Value{L"$EXEDIR", true, true});
            else if (decoded == 25) append(result, Value{L"$TEMP", true, true});
            else if (decoded == 26) append(result, Value{L"$PLUGINSDIR", true, true});
            else if (const auto it = state.variables.find(decoded); it != state.variables.end()) append(result, it->second);
            else append(result, unknown(L"$VAR" + std::to_wstring(decoded)));
        }
        (void)platform::utf8(result.text);
        return result;
    }

    void plan_files() {
        const auto stable_out = directory_flow();
        std::set<std::size_t> boundaries{0};
        bool indirect = false;
        const auto target = [&](std::int32_t address) {
            if (address < 0) { indirect = true; return; }
            if (address == 0) return;
            require(static_cast<std::size_t>(address) <= instructions.size(), Status::corrupt, L"NSIS 跳转目标越界。");
            boundaries.insert(static_cast<std::size_t>(address - 1));
        };
        for (std::size_t i = 0; i < instructions.size(); ++i) {
            const auto& e = instructions[i]; const auto& p = e.p;
            switch (e.op) {
            case 2: case 5: target(p[0]); break;
            case 12: case 34: target(p[1]); target(p[2]); break;
            case 14: target(p[0]); target(p[1]); break;
            case 22: target(p[3]); target(p[5]); break;
            case 26: target(p[2]); target(p[3]); break;
            case 28: target(p[2]); target(p[3]); target(p[4]); break;
            default: continue;
            }
            boundaries.insert(i + 1);
        }
        State state;
        std::map<std::pair<std::wstring, std::uint64_t>, std::size_t> duplicates;
        for (std::size_t i = 0; i < instructions.size(); ++i) {
            if (boundaries.contains(i) || indirect) state = State{};
            const auto& e = instructions[i]; const auto& p = e.p;
            if (e.op == 11 && p[1]) { state.out = string(p[0], state); continue; }
            if (e.op == 25) {
                require(p[0] >= 0 && p[0] < 16384, Status::corrupt, L"NSIS 变量索引无效。");
                auto value = string(p[1], state);
                // 只传播完整 StrCpy；截断/偏移需要运行时求值，保留为未知。
                if (p[2] != 0 || p[3] != 0) value = unknown(L"$VAR" + std::to_wstring(p[0]));
                if (p[0] == 22) state.out = value;
                else state.variables[static_cast<unsigned>(p[0])] = std::move(value);
                continue;
            }
            if (e.op == 20) {
                if (stable_out[i]) state.out = *stable_out[i];
                require(p[2] >= 0 && payloads.contains(static_cast<std::uint64_t>(p[2])), Status::corrupt, L"NSIS 文件指令未指向有效数据块边界。");
                const auto name = string(p[1], state);
                require(!name.text.empty(), Status::unsafe_path, L"NSIS 文件名为空。");
                Value value = name;
                if (!name.rooted) {
                    require(name.text.front() != L'\\' && name.text.front() != L'/' &&
                        !(name.text.size() > 1 && name.text[1] == L':'), Status::unsafe_path, L"NSIS 文件名包含绝对路径。");
                    value = state.out; append(value, Value{L"\\"}); append(value, name);
                }
                const auto offset = static_cast<std::uint64_t>(p[2]);
                if (const auto duplicate = duplicates.find({value.text, offset}); duplicate != duplicates.end()) {
                    catalog.files[duplicate->second].conditions += L", " + std::to_wstring(i);
                    continue;
                }
                require(catalog.files.size() < max_entries, Status::limit_exceeded, L"NSIS 文件条目超过 10000。");
                Entry file; file.id = L"nsis-" + std::to_wstring(i);
                file.source_expression = value.text;
                file.conditions = L"File instruction " + std::to_wstring(i) + L"; runtime conditions not evaluated";
                auto mapped = value.text;
                if (value.rooted) {
                    const auto end = mapped.find_first_of(L"\\/");
                    const auto root = mapped.substr(0, end);
                    std::wstring replacement;
                    if (root == L"$INSTDIR") replacement = L"app";
                    else if (root == L"$PLUGINSDIR") replacement = L"plugins";
                    else if (root == L"$TEMP") replacement = L"temp";
                    else if (root == L"$EXEDIR") replacement = L"exedir";
                    else replacement = root.substr(1);
                    mapped.replace(0, root.size(), replacement);
                }
                fs::path destination(mapped);
                platform::validate_relative(destination);
                if (!value.known) {
                    catalog.paths_resolved = false;
                    file.path_resolved = false;
                    destination = fs::path(L"_unresolved") / file.id / (name.known ? fs::path(name.text).filename() : fs::path(L"payload.bin"));
                }
                file.original_path = file.path = destination;
                duplicates.emplace(std::make_pair(value.text, offset), catalog.files.size());
                catalog.files.push_back(std::move(file)); file_offsets.push_back(offset);
                continue;
            }
            // 保守的基本块分析：跨调用/返回不猜路径，外部函数和插件可修改变量。
            if (e.op <= 5 || e.op == 44) { state = State{}; continue; }
            switch (e.op) {
            case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14:
            case 16: case 21: case 22: case 23: case 26: case 28: case 36: case 39: case 40:
            case 45: case 46: case 47: case 48: case 50: case 51: case 54: case 56: case 59: case 62:
                break;
            default:
                state.variables.clear();
                // 不完全支持的变量写入指令可能直接写 $OUTDIR。
                state.out = unknown(L"$OUTDIR");
                break;
            }
        }
        if (!catalog.paths_resolved) catalog.notes.push_back(L"无法静态确定原始目录的文件保留至 _unresolved；已识别卸载器的路径问题不影响应用载荷完成状态。");
    }

    std::vector<std::optional<Value>> directory_flow() {
        // 仅传播字节码显式 OUTDIR 赋值；合流必须一致。插件的任意变量副作用不模拟。
        const auto n = instructions.size();
        std::vector<std::vector<std::size_t>> next(n);
        std::set<std::size_t> roots{0};
        const auto section_stride = blocks[1].count ? (blocks[2].offset - blocks[1].offset) / blocks[1].count : 0;
        for (std::uint32_t i = 0; i < blocks[1].count; ++i) {
            const auto code = u32(header, blocks[1].offset + i * section_stride + 12);
            require(code < n, Status::corrupt, L"NSIS 区段入口越界。"); roots.insert(code);
        }
        bool indirect = false;
        for (std::size_t i = 0; i < n; ++i) {
            const auto& e = instructions[i]; const auto& p = e.p;
            const auto edge = [&](std::int32_t address) {
                if (address < 0) { indirect = true; return; }
                if (!address) { if (i + 1 < n) next[i].push_back(i + 1); return; }
                require(static_cast<std::size_t>(address) <= n, Status::corrupt, L"NSIS 跳转目标越界。");
                next[i].push_back(static_cast<std::size_t>(address - 1));
            };
            switch (e.op) {
            case 1: case 3: case 4: break;
            case 2: edge(p[0]); break;
            case 12: case 34: edge(p[1]); edge(p[2]); break;
            case 14: edge(p[0]); edge(p[1]); break;
            case 22: edge(p[3]); edge(p[5]); edge(0); break;
            case 26: edge(p[2]); edge(p[3]); break;
            case 28: edge(p[2]); edge(p[3]); edge(p[4]); break;
            case 5:
                if (p[0] <= 0) indirect = true;
                else { require(static_cast<std::size_t>(p[0]) <= n, Status::corrupt, L"NSIS 调用目标越界。"); roots.insert(static_cast<std::size_t>(p[0] - 1)); }
                edge(0); break;
            default: edge(0); break;
            }
        }
        std::vector<std::optional<Value>> result(n);
        if (indirect) return result;
        const auto writes_out = [](const Instruction& e) {
            if (e.op == 11) return e.p[1] != 0;
            if (e.op == 31) return e.p[1] && e.p[0] == 22;
            if (e.op == 42 || e.op == 43) return e.p[1] == 22 || e.p[2] == 22;
            switch (e.op) {
            case 15: case 17: case 18: case 19: case 24: case 25: case 27: case 29: case 30:
            case 32: case 33: case 35: case 38: case 52: case 55: case 57: case 60: case 61:
                return e.p[0] == 22;
            default: return false;
            }
        };
        // 对内部辅助函数仅证明“没有显式写 OUTDIR”；复杂调用保守回退。
        std::map<std::size_t, bool> preserves;
        std::size_t work = 0;
        const auto function_preserves = [&](std::size_t root) {
            if (const auto found = preserves.find(root); found != preserves.end()) return found->second;
            std::set<std::size_t> visited; std::vector<std::size_t> todo{root}; bool safe = true;
            while (!todo.empty() && safe) {
                const auto i = todo.back(); todo.pop_back(); if (!visited.insert(i).second) continue;
                require(++work <= 2000000, Status::limit_exceeded, L"NSIS 目录分析工作量超过上限。");
                const auto& e = instructions[i];
                if (writes_out(e)) { safe = false; break; }
                if (e.op == 5) todo.push_back(static_cast<std::size_t>(e.p[0] - 1));
                todo.insert(todo.end(), next[i].begin(), next[i].end());
            }
            preserves[root] = safe; return safe;
        };
        std::vector<Value> values;
        std::map<std::wstring, int> interned;
        std::vector<int> assigned(n, -1), states(n, -2);
        for (std::size_t i = 0; i < n; ++i) {
            const auto& e = instructions[i];
            if (!((e.op == 11 && e.p[1]) || (e.op == 25 && e.p[0] == 22 && !e.p[2] && !e.p[3]))) continue;
            auto value = string(e.p[e.op == 11 ? 0 : 1], State{});
            if (!value.known) continue;
            auto [entry, fresh] = interned.emplace(value.text, static_cast<int>(values.size()));
            if (fresh) values.push_back(std::move(value)); assigned[i] = entry->second;
        }
        std::deque<std::size_t> pending;
        std::vector<bool> queued(n, false);
        for (auto root : roots) { states[root] = -1; pending.push_back(root); queued[root] = true; }
        while (!pending.empty()) {
            const auto i = pending.front(); pending.pop_front(); queued[i] = false;
            require(++work <= 2000000, Status::limit_exceeded, L"NSIS 目录分析工作量超过上限。");
            auto value = states[i]; const auto& e = instructions[i];
            if (writes_out(e)) value = assigned[i];
            if (e.op == 5 && !function_preserves(static_cast<std::size_t>(e.p[0] - 1))) value = -1;
            for (auto target : next[i]) {
                const auto merged = states[target] == -2 ? value : (states[target] == value ? value : -1);
                if (states[target] == merged) continue;
                states[target] = merged;
                if (!queued[target]) { queued[target] = true; pending.push_back(target); }
            }
        }
        for (std::size_t i = 0; i < n; ++i) if (states[i] >= 0) result[i] = values[static_cast<std::size_t>(states[i])];
        catalog.notes.push_back(L"OUTDIR 按显式字节码赋值和分支合流还原；不模拟插件任意修改变量、脚本生成/下载内容或安装覆盖效果。");
        return result;
    }

    void measure(Payload& payload) {
        if (payload.method == Compression::stored) { payload.size = payload.bytes.size(); return; }
        const auto choices = method ? std::vector<Compression>{*method} : candidates(payload.bytes);
        for (auto candidate : choices) {
            try {
                codecs::Stream stream(candidate, payload.bytes);
                std::uint64_t total = 0;
                std::array<std::byte, 65536> buffer{};
                for (;;) {
                    const auto size = stream.read(buffer);
                    if (!size) break;
                    require(size <= max_output_bytes - total, Status::limit_exceeded, L"NSIS 单文件展开超过 8 GiB。");
                    total += size;
                }
                payload.size = total; payload.method = candidate; method = candidate; return;
            } catch (const Failure& failure) {
                if (failure.status != Status::corrupt || method) throw;
            }
        }
        throw Failure(Status::corrupt, L"NSIS 文件压缩流无效。");
    }

    fs::path extract(const fs::path& parent) {
        io::Output output(parent.empty() ? input.path().parent_path() : parent, input.path().stem().wstring());
        std::array<std::byte, 65536> buffer{};
        for (std::size_t i = 0; i < catalog.files.size(); ++i) {
            auto& entry = catalog.files[i]; const auto& payload = payloads.at(file_offsets[i]);
            log::Scope step(L"file.extract", nullptr, &entry.path);
            codecs::Stream stream(payload.method, payload.bytes);
            auto file = output.create_file(entry.path);
            std::uint64_t written = 0;
            while (written < entry.size) {
                const auto size = static_cast<std::size_t>((std::min)(entry.size - written, static_cast<std::uint64_t>(buffer.size())));
                auto bytes = std::span(buffer).first(size); stream.read_exact(bytes);
                platform::write_all(file.get(), bytes); written += size;
            }
            stream.finish();
            if (!FlushFileBuffers(file.get())) platform::io_failure(L"刷新 NSIS 输出文件失败");
            file.reset(); entry.sha256 = platform::sha256(output.full_path(entry.path));
            log::verified(entry);
        }
        const auto report = platform::utf8(catalog_json(catalog, true));
        {
            auto file = output.create_file(L"_extract-report.json");
            platform::write_all(file.get(), std::as_bytes(std::span(report)));
            if (!FlushFileBuffers(file.get())) platform::io_failure(L"保存 NSIS 解包报告失败");
        }
        return output.commit();
    }
};

NsisPackage::NsisPackage(const fs::path& input) : impl_(std::make_unique<Impl>(input)) {}
NsisPackage::~NsisPackage() = default;
const Catalog& NsisPackage::catalog() const { return impl_->catalog; }
fs::path NsisPackage::extract(const fs::path& parent) { return impl_->extract(parent); }
} // namespace extract::formats
