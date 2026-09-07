#include "formats/nsis.h"
#include "platform/log.h"
#include "core/progress.h"
#include "codecs/stream.h"
#include "io/output.h"
#include "io/temporary.h"
#include <algorithm>
#include <map>
#include <set>
#include <deque>

// 自行实现的磁盘解析器。布局依据 NSIS v312 / v251 的 fileform.h、fileform.cpp、
// fileform.c、exec.c、util.c；ANSI 转义另核对 build.cpp，不运行 NSIS 字节码。
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
    SolidData(Compression method, io::Bytes input) : file_(L"nsis-cache") {
        log::Scope step(L"nsis.decode_solid_cache");
        progress::Scope stage(progress::Phase::decoding, input.size(), L"正在展开固实压缩缓存，按压缩数据读取量");
        codecs::Stream stream(method, input);
        std::uint64_t consumed = 0;
        std::array<std::byte, 65536> buffer{};
        for (;;) {
            const auto size = stream.read(buffer);
            const auto used = stream.consumed();
            if (!size) { progress::advance(used - consumed); break; }
            file_.append(std::span(buffer).first(size));
            progress::advance(used - consumed); consumed = used;
        }
        require(!file_.bytes().empty(), Status::corrupt, L"NSIS Solid 数据为空。");
    }
    io::Bytes bytes() { return file_.bytes(); }
private:
    io::TemporaryFile file_;
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
    bool ansi = false;
    unsigned ansi_codepage = 0;
    std::vector<std::vector<std::int32_t>> languages;
    std::map<std::uint64_t, Payload> payloads;
    std::vector<std::uint64_t> file_offsets;
    std::vector<std::vector<std::size_t>> file_instructions;
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
            require(payload.size <= max_file_bytes - catalog.total_size, Status::limit_exceeded, L"NSIS 文件总输出超出 64 位文件范围。");
            catalog.total_size += payload.size;
        }
        require(!catalog.files.empty(), Status::unsupported, L"NSIS 包没有可提取的内嵌文件指令。");
        group_architecture_branches();
        plan_paths(catalog);
        catalog.notes.push_back(L"静态提取所有 File 指令；不执行条件、安装操作或插件，不生成卸载器或脚本写入的文件。");
        catalog.notes.push_back(L"files 记录本层文件；提取时会继续展开已识别的内嵌安装包，结果记录在 nestedPackages。--list 只列本层。");
        catalog.notes.push_back(solid ? L"Solid 压缩；使用临时磁盘缓存，按实际磁盘空间和地址空间检查。" : L"逐块压缩或不压缩。");
    }

    void parse_header(bool wide) {
        const std::size_t root_size = wide ? 332 : 300;
        io::Reader reader(header); reader.skip(4);
        for (auto& block : blocks) { block.offset = wide ? reader.u64() : reader.u32(); block.count = reader.u32(); }
        require(blocks[0].offset == root_size && blocks[1].offset >= blocks[0].offset &&
            blocks[2].offset >= blocks[1].offset && blocks[3].offset >= blocks[2].offset &&
            blocks[4].offset > blocks[3].offset && blocks[5].offset >= blocks[4].offset && blocks[5].offset <= header.size(),
            Status::unsupported, L"NSIS 块布局不属于受支持的格式。");
        require(blocks[2].count > 0 && blocks[2].count <= instruction_limit,
            Status::limit_exceeded, L"NSIS 指令或区段数量超出上限。");
        require(blocks[3].offset - blocks[2].offset == static_cast<std::uint64_t>(blocks[2].count) * 28,
            Status::corrupt, L"NSIS 指令表长度不一致。");
        if (blocks[1].count) {
            const auto section_size = (blocks[2].offset - blocks[1].offset) / blocks[1].count;
            ansi = section_size == 1048 || section_size == 8216;
            require((ansi || section_size == 2072 || section_size == 16408) &&
                section_size * blocks[1].count == blocks[2].offset - blocks[1].offset,
                Status::unsupported, L"NSIS 区段布局不属于受支持的 1024/8192 字符构建。");
        }
        const auto string_bytes = io::slice(header, blocks[3].offset, blocks[4].offset - blocks[3].offset);
        require((ansi || string_bytes.size() % 2 == 0) && string_bytes.size() >= 2, Status::corrupt, L"NSIS 字符串表长度无效。");
        io::Reader text_reader(string_bytes);
        while (text_reader.remaining()) strings += static_cast<wchar_t>(ansi ? text_reader.u8() : text_reader.u16());
        require(strings.front() == 0 && strings.back() == 0, Status::corrupt, L"NSIS 字符串表未终止。");
        if (ansi) {
            // 2.x 的 FC/FD/FE/FF 转义与 3.x ANSI 的 01/02/03/04 不同。
            // 先验证整个旧式字符串编码；不将未验证的 ANSI 变体套用此布局。
            bool encoded = false;
            for (std::size_t i = 0; i < strings.size(); ++i) {
                const auto c = strings[i];
                // 原始许可证字符串可含 02 + RTF，不能把任意低控制字节当成 3.x。
                const bool modern_reference = (c == 1 || c == 3) && i + 2 < strings.size()
                    && (strings[i + 1] & 128) && (strings[i + 2] & 128);
                require(!modern_reference, Status::unsupported, L"当前仅支持 NSIS 2.x ANSI 字符串编码。");
                if (c < 252) continue;
                require(++i < strings.size() && strings[i] != 0, Status::corrupt, L"NSIS ANSI 转义截断。");
                if (c == 252) continue;
                encoded = true;
                const auto low = strings[i];
                require(++i < strings.size() && strings[i] != 0, Status::corrupt, L"NSIS ANSI 编码参数截断。");
                require(c == 254 || ((low & 128) && (strings[i] & 128)), Status::corrupt, L"NSIS ANSI 编码参数无效。");
            }
            require(encoded, Status::unsupported, L"无法确认 NSIS ANSI 字符串编码。");
        }
        const std::wstring marker = ansi ? L"Nullsoft Install System v2." : L"Nullsoft Install System v3.";
        const auto version = strings.find(marker);
        if (version == strings.npos) {
            // BrandingText 可合法替换默认版本文本；结构校验不能依赖 UI 品牌字符串。
            catalog.format_version = ansi ? L"2.x ANSI (compatible layout)" : L"3.x Unicode (custom branding)";
            catalog.notes.push_back(L"默认 NSIS 版本文字已被替换；按块、指令和字符串布局识别，不猜测编译器补丁版本。");
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
        std::set<unsigned> codepages;
        for (std::uint32_t n = 0; n < blocks[4].count; ++n) {
            io::Reader language(io::slice(header, blocks[4].offset + static_cast<std::uint64_t>(n) * language_size, language_size));
            const auto language_id = language.u16();
            if (ansi) {
                DWORD codepage = 0;
                const auto locale = MAKELCID(language_id, SORT_DEFAULT);
                if (!GetLocaleInfoW(locale, LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                    reinterpret_cast<LPWSTR>(&codepage), sizeof(codepage) / sizeof(wchar_t)) || !IsValidCodePage(codepage)) codepage = 0;
                codepages.insert(codepage);
            }
            language.skip(8); std::vector<std::int32_t> values;
            while (language.remaining()) values.push_back(static_cast<std::int32_t>(language.u32()));
            languages.push_back(std::move(values));
        }
        if (ansi) {
            if (codepages.size() == 1) ansi_codepage = *codepages.begin();
            catalog.notes.push_back(L"ANSI 文本按安装包语言表推定代码页：" + std::to_wstring(ansi_codepage)
                + L"；代码页不确定时只接受 ASCII 路径，不使用当前系统代码页猜测。");
        }
        io::Reader code(io::slice(header, blocks[2].offset, static_cast<std::uint64_t>(blocks[2].count) * 28));
        for (std::uint32_t n = 0; n < blocks[2].count; ++n) {
            Instruction instruction{}; instruction.op = code.u32();
            for (auto& p : instruction.p) p = static_cast<std::int32_t>(code.u32());
            require(instruction.op > 0 && instruction.op <= (ansi ? 68U : 73U), Status::unsupported, L"NSIS 包包含未知指令编号。");
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
        std::string literal;
        const auto literal_byte = [&](wchar_t c) {
            require(literal.size() < 32000, Status::limit_exceeded, L"NSIS ANSI 字符串展开过长。");
            literal += static_cast<char>(c);
        };
        const auto flush_literal = [&] {
            if (literal.empty()) return;
            const bool ascii = std::all_of(literal.begin(), literal.end(), [](unsigned char c) { return c < 128; });
            require(ascii || ansi_codepage != 0, Status::unsupported, L"NSIS ANSI 路径的代码页不明确。");
            const auto page = ascii ? 1252U : ansi_codepage;
            const auto count = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, literal.data(), static_cast<int>(literal.size()), nullptr, 0);
            require(count > 0, Status::corrupt, L"NSIS ANSI 路径包含无效编码。");
            require(static_cast<std::size_t>(count) <= 16000 - result.text.size(), Status::limit_exceeded, L"NSIS 字符串展开过长。");
            std::wstring text(static_cast<std::size_t>(count), 0);
            require(MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, literal.data(), static_cast<int>(literal.size()), text.data(), count) == count,
                Status::corrupt, L"NSIS ANSI 路径解码失败。");
            append(result, Value{std::move(text)}); literal.clear();
        };
        for (std::size_t i = static_cast<std::size_t>(index);; ++i) {
            require(string_budget > 0, Status::limit_exceeded, L"NSIS 字符串解析工作量超过上限。");
            --string_budget;
            require(i < strings.size(), Status::corrupt, L"NSIS 字符串未终止。");
            auto c = strings[i];
            if (!c) break;
            if (ansi) {
                if (c < 252) { literal_byte(c); continue; }
                if (c == 252) {
                    require(++i < strings.size() && strings[i] != 0, Status::corrupt, L"NSIS ANSI 字符转义无效。");
                    literal_byte(strings[i]); continue;
                }
                flush_literal();
                c = static_cast<wchar_t>(256 - c); // FF lang / FE shell / FD var
            }
            if (c > 4) { append(result, Value{std::wstring(1, c)}); continue; }
            require(++i < strings.size(), Status::corrupt, L"NSIS 编码字符串截断。");
            auto value = static_cast<unsigned>(strings[i]);
            if (ansi) {
                require(value != 0 && ++i < strings.size() && strings[i] != 0, Status::corrupt, L"NSIS ANSI 编码字符串截断。");
                value |= static_cast<unsigned>(strings[i]) << 8;
            }
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
        flush_literal();
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
                    file_instructions[duplicate->second].push_back(i);
                    continue;
                }
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
                file_instructions.push_back({i});
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

    void group_architecture_branches() {
        // 只归组可证明的两个直线互斥分支。PE Machine 仅用于命名，非 PE 文件
        // 和共用的异构辅助程序仍按 File 指令所属分支保留，不按扩展名猜归属。
        struct NameLess {
            bool operator()(const std::wstring& a, const std::wstring& b) const {
                return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
            }
        };
        using Paths = std::map<std::wstring, std::size_t, NameLess>;
        const auto is_app = [](const Entry& file) {
            return file.path_resolved && !file.path.empty() && platform::equal_name(file.path.begin()->wstring(), L"app");
        };
        std::map<std::uint64_t, std::wstring> architectures;
        const auto architecture = [&](std::size_t index) -> std::wstring {
            const auto offset = file_offsets[index];
            if (const auto found = architectures.find(offset); found != architectures.end()) return found->second;
            const auto& payload = payloads.at(offset);
            std::array<std::byte, 4096> buffer{};
            auto bytes = std::span(buffer).first(static_cast<std::size_t>((std::min)(payload.size, static_cast<std::uint64_t>(buffer.size()))));
            codecs::Stream stream(payload.method, payload.bytes);
            stream.read_exact(bytes);
            std::wstring name;
            if (bytes.size() >= 64 && bytes[0] == std::byte{'M'} && bytes[1] == std::byte{'Z'}) {
                const auto pe = u32(bytes, 60);
                if (pe >= 64 && pe <= bytes.size() - 26 && u32(bytes, pe) == 0x4550) {
                    const auto machine = io::Reader(io::slice(bytes, pe + 4, 2)).u16();
                    const auto magic = io::Reader(io::slice(bytes, pe + 24, 2)).u16();
                    if (machine == 0x14c && magic == 0x10b) name = L"x86";
                    if (machine == 0x8664 && magic == 0x20b) name = L"x64";
                    if (machine == 0xaa64 && magic == 0x20b) name = L"arm64";
                }
            }
            architectures.emplace(offset, name); return name;
        };
        const auto n = instructions.size();
        std::size_t work = 0;
        for (std::size_t branch = 0; branch < n; ++branch) {
            const auto& condition = instructions[branch];
            std::int32_t a = 0, b = 0;
            if (condition.op == 26) { a = condition.p[2]; b = condition.p[3]; }
            else if (condition.op == 12 || condition.op == 34) { a = condition.p[1]; b = condition.p[2]; }
            else if (condition.op == 14) { a = condition.p[0]; b = condition.p[1]; }
            else continue;
            if (a < 0 || b < 0) continue;
            auto first = a ? static_cast<std::size_t>(a - 1) : branch + 1;
            auto second = b ? static_cast<std::size_t>(b - 1) : branch + 1;
            if (first > second) std::swap(first, second);
            if (first != branch + 1 || second <= first + 1 || second >= n) continue;
            const auto& jump = instructions[second - 1];
            if (jump.op != 2 || jump.p[0] <= 0) continue;
            const auto join = static_cast<std::size_t>(jump.p[0] - 1);
            if (join <= second || join >= n) continue;
            // 可选布局优化达到分析预算就保持原有无覆盖提取，不拒绝安装包。
            if (work + n + catalog.files.size() > 2000000) return;
            work += n + catalog.files.size();
            bool safe = true;
            for (auto i = first; i < join; ++i) {
                if (i == second - 1) continue;
                const auto op = instructions[i].op;
                if (op != 6 && op != 10 && op != 11 && op != 13 && op != 20 && op != 25) { safe = false; break; }
            }
            if (!safe) continue;
            // 其它分支、函数调用或区段不能从外部进入候选分支内部。
            const auto entry = [&](std::int32_t target) {
                if (target < 0) { safe = false; return; }
                if (target > 0 && static_cast<std::size_t>(target - 1) >= first && static_cast<std::size_t>(target - 1) < join) safe = false;
            };
            for (std::size_t i = 0; i < n && safe; ++i) {
                if (i >= branch && i < join) continue;
                const auto& e = instructions[i]; const auto& p = e.p;
                switch (e.op) {
                case 2: case 5: entry(p[0]); break;
                case 12: case 34: entry(p[1]); entry(p[2]); break;
                case 14: entry(p[0]); entry(p[1]); break;
                case 22: entry(p[3]); entry(p[5]); break;
                case 26: entry(p[2]); entry(p[3]); break;
                case 28: entry(p[2]); entry(p[3]); entry(p[4]); break;
                default: break;
                }
            }
            const auto stride = blocks[1].count ? (blocks[2].offset - blocks[1].offset) / blocks[1].count : 0;
            for (std::uint32_t i = 0; i < blocks[1].count && safe; ++i) {
                const auto code = u32(header, blocks[1].offset + i * stride + 12);
                if (code >= first && code < join) safe = false;
            }
            if (!safe) continue;
            std::vector<unsigned> masks(catalog.files.size());
            std::array<Paths, 2> paths;
            for (std::size_t i = 0; i < catalog.files.size() && safe; ++i) {
                if (!is_app(catalog.files[i])) continue;
                for (const auto instruction : file_instructions[i]) {
                    masks[i] |= instruction >= first && instruction < second ? 1U : instruction >= second && instruction < join ? 2U : 4U;
                }
                for (unsigned arm = 0; arm < 2; ++arm) {
                    if (!(masks[i] & (1U << arm))) continue;
                    const auto [it, fresh] = paths[arm].emplace(catalog.files[i].path.wstring(), i);
                    if (!fresh && file_offsets[it->second] != file_offsets[i]) safe = false;
                }
            }
            if (!safe) continue;
            std::array<std::wstring, 2> names;
            bool distinct = false;
            for (const auto& [path, left] : paths[0]) {
                const auto right = paths[1].find(path);
                if (right == paths[1].end() || file_offsets[left] == file_offsets[right->second]) continue;
                const auto x = architecture(left), y = architecture(right->second);
                // 两个分支也可能各自带有同架构的不同资源 DLL；它们不能用作架构证据。
                if (x == y) continue;
                if (x.empty() || y.empty() || (!names[0].empty() && names[0] != x) || (!names[1].empty() && names[1] != y)) { safe = false; break; }
                names = {x, y}; distinct = true;
            }
            if (!safe || !distinct) continue;
            std::vector<Entry> files;
            std::vector<std::uint64_t> offsets;
            Paths emitted;
            const auto emit = [&](Entry file, std::uint64_t offset) {
                const auto [it, fresh] = emitted.emplace(file.path.wstring(), files.size());
                if (!fresh) { if (offsets[it->second] != offset) safe = false; return; }
                platform::validate_relative(file.path);
                files.push_back(std::move(file)); offsets.push_back(offset);
            };
            for (std::size_t i = 0; i < catalog.files.size(); ++i) {
                if (!is_app(catalog.files[i])) { emit(catalog.files[i], file_offsets[i]); continue; }
                const auto mask = masks[i] & 4 ? 3U : masks[i];
                for (unsigned arm = 0; arm < 2; ++arm) if (mask & (1U << arm)) {
                    auto file = catalog.files[i];
                    fs::path path(L"app-" + names[arm]);
                    for (auto part = std::next(file.path.begin()); part != file.path.end(); ++part) path /= *part;
                    file.path = std::move(path); file.id += L"-" + names[arm];
                    file.conditions += L"; static branch layout " + names[arm] + L" (runtime condition not evaluated)";
                    emit(std::move(file), file_offsets[i]);
                }
            }
            for (const auto& file : files) {
                for (auto parent = file.path.parent_path(); !parent.empty(); parent = parent.parent_path())
                    if (emitted.contains(parent.wstring())) safe = false;
            }
            if (!safe) continue;
            std::uint64_t total = 0;
            for (const auto& file : files) total = checked_size_sum(total, file.size);
            catalog.files = std::move(files); file_offsets = std::move(offsets); catalog.total_size = total;
            catalog.notes.push_back(L"已将可静态证明的双分支架构布局分别保存至 app-" + names[0] + L" 和 app-" + names[1]
                + L"；各分支的普通资源与共用文件一并保留，不执行架构判断或安装脚本。");
            return;
        }
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
                progress::Scope stage(progress::Phase::analyzing, payload.bytes.size(), L"正在测量文件，按压缩数据读取量");
                codecs::Stream stream(candidate, payload.bytes);
                std::uint64_t total = 0;
                std::uint64_t consumed = 0;
                std::array<std::byte, 65536> buffer{};
                for (;;) {
                    const auto size = stream.read(buffer);
                    if (!size) break;
                    require(size <= max_file_bytes - total, Status::limit_exceeded, L"NSIS 单文件展开超出 64 位文件范围。");
                    total += size;
                    progress::advance(stream.consumed() - consumed); consumed = stream.consumed();
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
            progress::file(entry.path.native());
            log::Scope step(L"file.extract", nullptr, &entry.path);
            codecs::Stream stream(payload.method, payload.bytes);
            auto file = output.create_file(entry.path);
            std::uint64_t written = 0;
            while (written < entry.size) {
                const auto size = static_cast<std::size_t>((std::min)(entry.size - written, static_cast<std::uint64_t>(buffer.size())));
                auto bytes = std::span(buffer).first(size); stream.read_exact(bytes);
                platform::write_payload(file.get(), bytes); written += size;
            }
            stream.finish();
            if (!FlushFileBuffers(file.get())) platform::io_failure(L"刷新 NSIS 输出文件失败");
            file.reset(); entry.sha256 = platform::sha256(output.full_path(entry.path));
            log::verified(entry);
        }
        progress::Scope finalizing(progress::Phase::finalizing);
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
