#include "formats/inno.h"
#include "platform/log.h"
#include "core/progress.h"
#include "formats/pe.h"
#include "codecs/stream.h"
#include "io/output.h"
#include <algorithm>
#include <map>
#include <numeric>

// 磁盘布局依据：jrsoftware/issrc is-6_2_2、is-6_5_4、is-6_6_1、is-6_7_3、is-7_1_0 的 Struct / Shared.Struct、
// Shared.SetupEntFunc、Compression.Base 和 Setup.FileExtractor。
// 此处按字节读取磁盘记录，不映射 Delphi 内存结构，也不执行安装脚本。
namespace extract::formats {
namespace {
constexpr std::size_t metadata_limit = 64 * 1024 * 1024;

std::wstring hex_string(io::Bytes bytes) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto c = std::to_integer<unsigned>(byte);
        result += digits[c >> 4]; result += digits[c & 15];
    }
    return result;
}

std::vector<std::byte> metadata_block(io::Reader& source, bool wide_size) {
    const auto expected_crc = source.u32();
    const auto header = source.take(wide_size ? 9 : 5);
    require(codecs::crc32(header) == expected_crc, Status::corrupt, L"Inno 元数据块头 CRC 不匹配。");
    io::Reader fields(header);
    const auto stored = wide_size ? fields.u64() : fields.u32();
    const auto compressed = fields.u8();
    require(compressed <= 1, Status::corrupt, L"Inno 元数据压缩标志无效。");
    require(stored <= metadata_limit, Status::limit_exceeded, L"Inno 元数据压缩块超过 64 MiB。");
    io::Reader chunks(source.take(static_cast<std::size_t>(stored)));
    std::vector<std::byte> joined;
    joined.reserve(static_cast<std::size_t>(stored));
    while (chunks.remaining()) {
        const auto crc = chunks.u32();
        require(chunks.remaining() != 0, Status::corrupt, L"Inno 元数据 CRC 后缺少内容。");
        const auto data = chunks.take((std::min)(chunks.remaining(), std::size_t{4096}));
        require(codecs::crc32(data) == crc, Status::corrupt, L"Inno 元数据内容 CRC 不匹配。");
        joined.insert(joined.end(), data.begin(), data.end());
    }
    return codecs::decode_bounded(compressed ? codecs::Compression::lzma1 : codecs::Compression::stored,
                                 joined, metadata_limit);
}

std::vector<std::wstring> strings(io::Reader& reader, std::size_t unicode_count, std::size_t ansi_count = 0) {
    std::vector<std::wstring> result;
    for (std::size_t i = 0; i < unicode_count; ++i) {
        const auto size = reader.u32();
        require(size % 2 == 0, Status::corrupt, L"Inno UTF-16 字符串长度无效。");
        io::Reader characters(reader.take(size));
        std::wstring text;
        text.reserve(size / 2);
        while (characters.remaining()) text += static_cast<wchar_t>(characters.u16());
        require(text.find(L'\0') == text.npos, Status::corrupt, L"Inno 字符串包含空字符。");
        (void)platform::utf8(text);
        result.push_back(std::move(text));
    }
    for (std::size_t i = 0; i < ansi_count; ++i) reader.skip(reader.u32());
    return result;
}

fs::path destination_path(const std::wstring& expression, const std::wstring& id, Catalog& catalog, bool& resolved) {
    require(!expression.empty() && expression.size() < 16000, Status::unsafe_path, L"Inno 目标路径为空或过长。");
    require(expression[0] != L'\\' && expression[0] != L'/' && !(expression.size() > 1 && expression[1] == L':'),
            Status::unsafe_path, L"Inno 目标包含绝对磁盘路径。");
    std::wstring mapped;
    bool unresolved = false;
    for (std::size_t i = 0; i < expression.size();) {
        if (expression[i] != L'{') { mapped += expression[i++]; continue; }
        if (i + 1 < expression.size() && expression[i + 1] == L'{') { mapped += L'{'; i += 2; continue; }
        const auto end = expression.find(L'}', i + 1);
        require(end != expression.npos, Status::corrupt, L"Inno 目录常量缺少结束符。");
        const auto constant = expression.substr(i + 1, end - i - 1);
        const bool simple = !constant.empty() && std::all_of(constant.begin(), constant.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_';
        });
        if (i == 0 && simple && (end + 1 == expression.size() || expression[end + 1] == L'\\' || expression[end + 1] == L'/'))
            mapped += constant;
        else { mapped += L"_dynamic_"; unresolved = true; }
        i = end + 1;
    }
    fs::path logical(mapped);
    platform::validate_relative(logical);
    if (unresolved) {
        catalog.paths_resolved = false;
        resolved = false;
        // 保留载荷与完整原表达式，不运行 code/cm/参数表达式猜测最终路径。
        return fs::path(L"_unresolved") / id / logical.filename();
    }
    return logical;
}

void undo_call_filter(std::span<std::byte> data, std::uint32_t file_offset) {
    // Inno 的 64 KiB 分段 CALL/JMP 编码是独立格式规则，不等同于标准 BCJ。
    // 参考 Compression.Base.pas 的 v3 算法；原作者与许可见 third_party/notices。
    if (data.size() < 5) return;
    for (std::size_t opcode = 0; opcode + 4 < data.size();) {
        const auto code = std::to_integer<unsigned>(data[opcode]);
        if (code != 0xe8 && code != 0xe9) { ++opcode; continue; }
        const auto operand = opcode + 1;
        const auto high = std::to_integer<unsigned>(data[operand + 3]);
        if (high == 0 || high == 255) {
            const auto absolute = std::to_integer<std::uint32_t>(data[operand])
                | (std::to_integer<std::uint32_t>(data[operand + 1]) << 8)
                | (std::to_integer<std::uint32_t>(data[operand + 2]) << 16);
            const auto relative = (absolute - file_offset - static_cast<std::uint32_t>(operand) - 4) & 0xffffffu;
            for (unsigned j = 0; j < 3; ++j) data[operand + j] = static_cast<std::byte>((relative >> (j * 8)) & 255);
            if (relative & 0x800000u) data[operand + 3] ^= std::byte{255};
        }
        opcode += 5;
    }
}
}

struct InnoPackage::Impl {
    io::Input input;
    Catalog catalog;
    enum class Schema { v610u, v650, v652, v661, v670, v7003 } schema = Schema::v670;
    bool version7 = false;
    std::uint64_t payload_offset = 0, metadata_offset = 0;
    codecs::Compression method = codecs::Compression::stored;
    struct Location {
        std::uint64_t start, suboffset, size, packed;
        std::wstring hash;
        std::uint8_t flags;
        std::vector<std::size_t> files;
    };
    std::vector<Location> locations;
    struct Chunk { std::uint64_t packed = 0, expanded = 0; bool compressed = false; std::vector<std::size_t> locations; };
    std::map<std::uint64_t, Chunk> chunks;

    explicit Impl(const fs::path& path) : input(path) {
        catalog.input = input.path(); catalog.format = L"Inno Setup";
        const auto table_bytes = pe_rcdata(input.bytes(), 11111);
        require(table_bytes.size() >= 16, Status::unsupported, L"PE 资源不是 Inno 偏移表。");
        constexpr std::array<unsigned char, 12> signature{'r','D','l','P','t','S',0xcd,0xe6,0xd7,0x7b,0x0b,0x2a};
        for (std::size_t i = 0; i < signature.size(); ++i)
            require(std::to_integer<unsigned char>(table_bytes[i]) == signature[i], Status::unsupported, L"PE 资源不是 Inno 偏移表。");
        io::Reader table(table_bytes); table.skip(12);
        const auto loader_version = table.u32();
        require(loader_version == 1 || loader_version == 2, Status::unsupported,
                L"尚未支持 Inno loader 版本：" + std::to_wstring(loader_version));
        const bool loader64 = loader_version == 2;
        require(table_bytes.size() == (loader64 ? 64U : 44U), Status::corrupt, L"Inno loader 偏移表长度错误。");
        const auto declared_size = loader64 ? table.u64() : table.u32();
        table.skip(loader64 ? 16 : 12);
        metadata_offset = loader64 ? table.u64() : table.u32();
        payload_offset = loader64 ? table.u64() : table.u32();
        if (loader64) table.skip(4);
        require(table.u32() == codecs::crc32(table_bytes.first(loader64 ? 60 : 40)), Status::corrupt, L"Inno loader 偏移表 CRC 不匹配。");
        require(declared_size <= input.bytes().size() && metadata_offset < declared_size,
                Status::corrupt, L"Inno 声明长度或元数据偏移越界。");
        require(payload_offset != 0, Status::unsupported, L"当前不支持 Inno 外置分卷。");
        require(payload_offset <= metadata_offset, Status::corrupt, L"Inno 载荷范围无效。");
        io::Reader source(io::slice(input.bytes(), metadata_offset, declared_size - metadata_offset));
        const auto id_bytes = source.take(64);
        std::wstring version;
        bool terminated = false;
        for (const auto byte : id_bytes) {
            const auto c = std::to_integer<unsigned char>(byte);
            if (c == 0) terminated = true;
            else {
                require(!terminated && c < 128, Status::corrupt, L"Inno 数据版本标识损坏。");
                version += c;
            }
        }
        if (version == L"Inno Setup Setup Data (6.1.0) (u)") { schema = Schema::v610u; catalog.format_version = L"6.1.0 (u)"; }
        else if (version == L"Inno Setup Setup Data (6.5.0)") { schema = Schema::v650; catalog.format_version = L"6.5.0"; }
        else if (version == L"Inno Setup Setup Data (6.5.2)") { schema = Schema::v652; catalog.format_version = L"6.5.2"; }
        else if (version == L"Inno Setup Setup Data (6.6.1)") { schema = Schema::v661; catalog.format_version = L"6.6.1"; }
        else if (version == L"Inno Setup Setup Data (6.7.0)") { schema = Schema::v670; catalog.format_version = L"6.7.0"; }
        else if (version == L"Inno Setup Setup Data (7.0.0.3)") { schema = Schema::v7003; catalog.format_version = L"7.0.0.3"; }
        else throw Failure(Status::unsupported, L"尚未支持此 Inno 数据版本：" + version);
        require(loader64 == (schema != Schema::v610u), Status::unsupported, L"Inno loader 与数据版本的组合尚未支持：" + version);
        version7 = schema == Schema::v7003;
        if (schema != Schema::v610u) {
            const auto encryption_crc = source.u32();
            const auto encryption = source.take(49);
            require(codecs::crc32(encryption) == encryption_crc, Status::corrupt, L"Inno 加密描述 CRC 不匹配。");
            require(encryption[0] == std::byte{0}, Status::unsupported, L"当前不支持加密的 Inno 安装包。");
        }
        const auto metadata = metadata_block(source, schema >= Schema::v670);
        const auto location_data = metadata_block(source, schema >= Schema::v670);
        read_catalog(metadata, location_data);
    }

    void read_catalog(io::Bytes metadata, io::Bytes location_data) {
        io::Reader reader(metadata);
        const bool modern = schema >= Schema::v670;
        const bool legacy = schema == Schema::v610u;
        (void)strings(reader, legacy ? 30 : (modern ? 39 : 34), 4);
        std::array<std::uint32_t, 17> counts{};
        for (std::size_t i = 0; i < counts.size(); ++i) {
            if (legacy && i == 7) continue; // 此布局没有 ISSigKey 表
            counts[i] = reader.u32();
            require(counts[i] <= (i == 9 ? location_data.size() : metadata.size()), Status::corrupt, L"Inno 表条目数超过实际元数据范围。");
        }
        if (version7) reader.skip(4); // CompiledCodeVersion
        reader.skip(legacy ? 74 : (modern ? 65 : (schema == Schema::v661 ? 55 : (schema == Schema::v650 ? 38 : 46))));
        require(reader.u32() == 1, Status::unsupported, L"当前不支持 Inno 多分卷媒体布局。");
        reader.skip(6);
        const auto compression = reader.u8();
        require(compression <= 4, Status::unsupported, L"Inno 使用了未知压缩方法。");
        method = static_cast<codecs::Compression>(compression);
        if (legacy) {
            reader.skip(12); // 架构、目录页设置和显示大小
            const auto options = reader.take(6);
            require((std::to_integer<unsigned>(options[4]) & 0x10) == 0, Status::unsupported, L"当前不支持加密的 Inno 安装包。");
        } else reader.skip(modern ? 18 : 16);
        struct Layout { unsigned strings, ansi, tail; };
        std::array<Layout, 8> prefix{{{4,4,19}, {2,0,4}, {0,1,0}, {4,0,30},
            {5,0,39}, {6,0,23}, {7,0,27}, {3,0,0}}};
        if (!modern) { prefix[4].tail = 42; prefix[5].tail = 26; }
        if (schema <= Schema::v652) prefix[0] = {6,4,21};
        for (std::size_t table = 0; table < prefix.size(); ++table) {
            for (std::uint32_t i = 0; i < counts[table]; ++i) {
                (void)strings(reader, prefix[table].strings, prefix[table].ansi);
                reader.skip(prefix[table].tail);
            }
        }
        std::vector<std::uint32_t> location_indices;
        unsigned generated = 0;
        for (std::uint32_t i = 0; i < counts[8]; ++i) {
            auto text = strings(reader, legacy ? 10 : 15, legacy ? 0 : 1);
            reader.skip(legacy ? 20 : 33 + 20); // 外部验证描述、版本约束
            const auto location = reader.u32();
            reader.skip(4 + 8 + 2);
            if (version7) reader.skip(1);
            reader.skip(legacy ? 4 : (modern ? 8 : 5));
            const auto type = reader.u8();
            require(type <= 1, Status::corrupt, L"Inno 文件类型无效。");
            if (type == 1) {
                require(location == 0xffffffffu, Status::unsupported, L"未知的 Inno 卸载程序记录。");
                ++generated;
                continue;
            }
            require(location != 0xffffffffu, Status::unsupported, L"安装包含需要外部文件或下载的条目，当前不能完整提取。");
            require(location < counts[9], Status::corrupt, L"Inno 文件引用了不存在的位置条目。");
            Entry file;
            file.id = L"inno-" + std::to_wstring(i + 1);
            file.source_expression = text[1];
            file.original_path = file.path = destination_path(text[1], file.id, catalog, file.path_resolved);
            constexpr std::array<const wchar_t*, 4> condition_names{L"Components=", L"Tasks=", L"Languages=", L"Check="};
            for (std::size_t c = 0; c < 4; ++c) if (!text[c + 4].empty()) {
                if (!file.conditions.empty()) file.conditions += L"; ";
                file.conditions += condition_names[c] + text[c + 4];
            }
            catalog.files.push_back(std::move(file));
            location_indices.push_back(location);
        }
        require(!catalog.files.empty(), Status::unsupported, L"此 Inno 安装包没有内嵌静态文件。");
        if (generated) catalog.notes.push_back(L"安装时生成的卸载程序记录未作为内嵌文件提取：" + std::to_wstring(generated));
        if (!catalog.paths_resolved) catalog.notes.push_back(L"存在动态目录表达式，文件保存在 _unresolved，原表达式记录在 sourceExpression。");
        require(location_data.size() == static_cast<std::uint64_t>(counts[9]) * (legacy ? 74 : (schema == Schema::v650 ? 85 : 89)), Status::corrupt, L"Inno 文件位置表长度错误。");
        io::Reader records(location_data);
        for (std::uint32_t i = 0; i < counts[9]; ++i) {
            const auto first = records.u32(), last = records.u32();
            require(first == 0 && last == 0, Status::unsupported, L"当前不支持 Inno 跨卷文件。");
            Location location{};
            location.start = schema <= Schema::v650 ? records.u32() : records.u64(); location.suboffset = records.u64();
            location.size = records.u64(); location.packed = records.u64();
            location.hash = hex_string(records.take(legacy ? 20 : 32));
            records.skip(16);
            if (legacy) {
                const auto flags = records.u16();
                require((flags & ~0x7ffu) == 0, Status::corrupt, L"Inno 文件位置标志无效。");
                // 旧版 16 位 flags 的 CALL、加密、压缩位分别为 4、6、7。
                location.flags = static_cast<std::uint8_t>(((flags & 0x10) >> 2) | ((flags & 0xc0) >> 3));
            } else location.flags = records.u8();
            require((location.flags & ~31u) == 0, Status::corrupt, L"Inno 文件位置标志无效。");
            require((location.flags & 8) == 0, Status::unsupported, L"当前不支持 Inno 加密文件。");
            require(location.suboffset <= max_file_bytes && location.size <= max_file_bytes - location.suboffset,
                    Status::limit_exceeded, L"Inno 数据块展开超出 64 位文件范围。");
            locations.push_back(std::move(location));
        }
        for (std::size_t i = 0; i < catalog.files.size(); ++i) {
            auto& location = locations[location_indices[i]];
            location.files.push_back(i);
            auto& file = catalog.files[i];
            file.size = location.size;
            if (legacy) file.expected_sha1 = location.hash;
            else file.expected_sha256 = location.hash;
            require(file.size <= max_file_bytes - catalog.total_size, Status::limit_exceeded, L"Inno 总输出超出 64 位文件范围。");
            catalog.total_size += file.size;
        }
        const auto payload = io::slice(input.bytes(), payload_offset, metadata_offset - payload_offset);
        for (std::size_t i = 0; i < locations.size(); ++i) {
            const auto& location = locations[i];
            require(!location.files.empty(), Status::corrupt, L"Inno 位置表含未被文件清单引用的载荷。");
            require(location.start <= payload.size() && payload.size() - location.start >= 4
                    && location.packed <= payload.size() - location.start - 4, Status::corrupt, L"Inno 文件数据块越界。");
            io::Reader signature(io::slice(payload, location.start, 4));
            require(signature.u32() == 0x1a626c7a, Status::corrupt, L"Inno 文件数据块标识损坏。");
            auto [it, inserted] = chunks.try_emplace(location.start);
            auto& chunk = it->second;
            const bool compressed = (location.flags & 16) != 0;
            if (inserted) { chunk.packed = location.packed; chunk.compressed = compressed; }
            require(chunk.packed == location.packed && chunk.compressed == compressed, Status::corrupt, L"Inno 同一数据块描述不一致。");
            chunk.expanded = (std::max)(chunk.expanded, location.suboffset + location.size);
            chunk.locations.push_back(i);
        }
        std::uint64_t input_end = 0, expanded_total = 0;
        for (auto& [start, chunk] : chunks) {
            require(start >= input_end, Status::corrupt, L"Inno 压缩数据块互相重叠。");
            input_end = start + 4 + chunk.packed;
            require(chunk.expanded <= max_file_bytes - expanded_total, Status::limit_exceeded, L"Inno 数据块累计展开超出 64 位文件范围。");
            expanded_total += chunk.expanded;
            std::stable_sort(chunk.locations.begin(), chunk.locations.end(), [&](auto a, auto b) {
                if (locations[a].suboffset != locations[b].suboffset) return locations[a].suboffset < locations[b].suboffset;
                return locations[a].size < locations[b].size; // 同偏移的空文件先处理
            });
            std::uint64_t end = 0;
            for (const auto index : chunk.locations) {
                const auto& location = locations[index];
                require(location.suboffset >= end, Status::corrupt, L"Inno 文件数据范围互相重叠。");
                end = location.suboffset + location.size;
            }
        }
        plan_paths(catalog);
    }

    fs::path extract(const fs::path& parent) {
        io::Output output(parent.empty() ? input.path().parent_path() : parent, input.path().stem().wstring());
        std::array<std::byte, 65536> buffer{};
        for (const auto& [start, chunk] : chunks) {
            log::Scope chunk_step(L"inno.decode_chunk");
            log::detail(log::Level::info, L"inno.chunk", [&] { return L"offset=" + std::to_wstring(start) + L"; packedBytes=" + std::to_wstring(chunk.packed); });
            const auto data = io::slice(input.bytes(), payload_offset + start + 4, chunk.packed);
            codecs::Stream stream(chunk.compressed ? method : codecs::Compression::stored, data);
            std::uint64_t position = 0;
            for (const auto index : chunk.locations) {
                const auto& location = locations[index];
                if (position < location.suboffset) {
                    progress::Scope stage(progress::Phase::decoding, location.suboffset - position, L"跳过数据块中的未提取内容");
                    while (position < location.suboffset) {
                        const auto size = static_cast<std::size_t>((std::min)(location.suboffset - position, static_cast<std::uint64_t>(buffer.size())));
                        stream.read_exact(std::span(buffer).first(size)); position += size;
                        progress::advance(size);
                    }
                }
                std::vector<platform::Handle> files;
                if (!location.files.empty()) progress::file(catalog.files[location.files.front()].path.native());
                for (auto entry : location.files) files.push_back(output.create_file(catalog.files[entry].path));
                std::uint64_t written = 0;
                while (written < location.size) {
                    const auto size = static_cast<std::size_t>((std::min)(location.size - written, static_cast<std::uint64_t>(buffer.size())));
                    auto bytes = std::span(buffer).first(size);
                    stream.read_exact(bytes);
                    if (location.flags & 4) undo_call_filter(bytes, static_cast<std::uint32_t>(written));
                    for (const auto& file : files) platform::write_payload(file.get(), bytes);
                    written += size; position += size;
                }
                for (auto& file : files) {
                    if (!FlushFileBuffers(file.get())) platform::io_failure(L"刷新 Inno 输出文件失败");
                    file.reset();
                }
                for (auto entry : location.files) {
                    auto& file = catalog.files[entry];
                    log::Scope step(L"file.verify", nullptr, &file.path);
                    file.sha256 = platform::sha256(output.full_path(file.path));
                    const bool verified = file.expected_sha1.empty() ? file.sha256 == file.expected_sha256 :
                        platform::sha1(output.full_path(file.path)) == file.expected_sha1;
                    require(verified, Status::corrupt, L"Inno 文件包内摘要校验失败：" + file.source_expression);
                    file.source_hash_verified = true;
                    log::verified(file);
                }
            }
            stream.finish();
        }
        progress::Scope finalizing(progress::Phase::finalizing);
        const auto report = platform::utf8(catalog_json(catalog, true));
        {
            auto file = output.create_file(L"_extract-report.json");
            platform::write_all(file.get(), std::as_bytes(std::span(report)));
            if (!FlushFileBuffers(file.get())) platform::io_failure(L"保存 Inno 解包报告失败");
        }
        return output.commit();
    }
};
InnoPackage::InnoPackage(const fs::path& input) : impl_(std::make_unique<Impl>(input)) {}
InnoPackage::~InnoPackage() = default;
const Catalog& InnoPackage::catalog() const { return impl_->catalog; }
fs::path InnoPackage::extract(const fs::path& parent) { return impl_->extract(parent); }
} // namespace extract::formats
