#include "formats/pe.h"
#include <algorithm>

namespace extract::formats {
PeLayout pe_layout(io::Bytes input) {
    io::Reader dos(io::slice(input, 0, 64));
    require(dos.u16() == 0x5a4d, Status::unsupported, L"输入不是 PE。");
    dos.skip(58); const auto offset = dos.u32();
    io::Reader header(io::slice(input, offset, 24));
    require(header.u32() == 0x4550, Status::corrupt, L"PE 签名损坏。");
    header.skip(2); const auto count = header.u16(); header.skip(12); const auto optional_size = header.u16();
    require(count && count <= 96, Status::corrupt, L"PE 节数量无效。");
    const auto optional_offset = static_cast<std::uint64_t>(offset) + 24;
    const auto optional = io::slice(input, optional_offset, optional_size);
    io::Reader magic_reader(optional); const auto magic = magic_reader.u16();
    require(magic == 0x10b || magic == 0x20b, Status::unsupported, L"不支持此 PE 架构。");
    io::Reader size_reader(io::slice(optional, 60, 4));
    PeLayout result; const auto header_size = size_reader.u32(); result.overlay = header_size;
    require(result.overlay >= optional_offset + optional_size + count * 40ULL && result.overlay <= input.size(),
            Status::corrupt, L"PE 头部长度无效。");
    const std::size_t directory_offset = magic == 0x10b ? 92 : 108;
    require(directory_offset <= optional.size(), Status::corrupt, L"PE 可选头截断。");
    io::Reader directories(optional.subspan(directory_offset));
    if (directories.u32() >= 5) {
        directories.skip(32); result.certificate_offset = directories.u32(); result.certificate_size = directories.u32();
        require((result.certificate_offset == 0) == (result.certificate_size == 0), Status::corrupt, L"PE 证书目录无效。");
        (void)io::slice(input, result.certificate_offset, result.certificate_size);
    }
    io::Reader table(io::slice(input, optional_offset + optional_size, count * 40ULL));
    for (unsigned i = 0; i < count; ++i) {
        auto name_bytes = table.take(8); std::string name;
        for (const auto b : name_bytes) { if (b == std::byte{}) break; name += static_cast<char>(b); }
        table.skip(8); const auto size = table.u32(), raw = table.u32(); table.skip(16);
        if (size) {
            require(raw >= header_size, Status::corrupt, L"PE 节覆盖头部。");
            (void)io::slice(input, raw, size);
            for (const auto& s : result.sections)
                require(!s.size || static_cast<std::uint64_t>(raw) + size <= s.offset ||
                        static_cast<std::uint64_t>(s.offset) + s.size <= raw, Status::corrupt, L"PE 节数据重叠。");
            result.overlay = (std::max)(result.overlay, static_cast<std::uint64_t>(raw) + size);
        }
        result.sections.push_back({std::move(name), raw, size});
    }
    return result;
}

io::Bytes pe_rcdata(io::Bytes input, std::uint32_t id) {
    io::Reader dos(io::slice(input, 0, 64));
    require(dos.u16() == 0x5a4d, Status::unsupported, L"输入不是受支持的 PE 安装包。");
    dos.skip(58);
    const auto pe_offset = dos.u32();
    io::Reader header(io::slice(input, pe_offset, 24));
    require(header.u32() == 0x4550, Status::corrupt, L"PE 签名损坏。");
    header.skip(2);
    const auto section_count = header.u16();
    header.skip(12);
    const auto optional_size = header.u16();
    require(section_count > 0 && section_count <= 96, Status::corrupt, L"PE 节数量无效。");
    const std::uint64_t optional_offset = static_cast<std::uint64_t>(pe_offset) + 24;
    io::Reader optional(io::slice(input, optional_offset, optional_size));
    const auto magic = optional.u16();
    require(magic == 0x10b || magic == 0x20b, Status::unsupported, L"不支持此 PE 可选头格式。");
    optional.skip(magic == 0x10b ? 90 : 106);
    require(optional.u32() >= 3, Status::unsupported, L"PE 没有资源目录。");
    optional.skip(16);
    const auto resource_rva = optional.u32();
    const auto resource_size = optional.u32();
    require(resource_rva != 0 && resource_size != 0, Status::unsupported, L"PE 没有资源目录。");
    struct Section { std::uint32_t rva, size, offset; };
    std::vector<Section> sections;
    io::Reader table(io::slice(input, optional_offset + optional_size, static_cast<std::uint64_t>(section_count) * 40));
    for (unsigned i = 0; i < section_count; ++i) {
        table.skip(12);
        const auto rva = table.u32(), size = table.u32(), offset = table.u32();
        table.skip(16);
        (void)io::slice(input, offset, size);
        sections.push_back({rva, size, offset});
    }
    const auto resolve = [&](std::uint32_t rva, std::uint32_t size) {
        io::Bytes result;
        bool found = false;
        for (const auto& section : sections) {
            if (rva < section.rva || static_cast<std::uint64_t>(rva) - section.rva > section.size) continue;
            const auto delta = rva - section.rva;
            if (size > section.size - delta) continue;
            require(!found, Status::corrupt, L"PE 资源位于重叠的节内。");
            result = io::slice(input, static_cast<std::uint64_t>(section.offset) + delta, size);
            found = true;
        }
        require(found, Status::corrupt, L"PE 资源 RVA 越界。");
        return result;
    };
    const auto resources = resolve(resource_rva, resource_size);
    const auto child = [&](std::uint32_t offset, std::optional<std::uint32_t> name, bool directory) {
        io::Reader node(io::slice(resources, offset, 16));
        node.skip(12);
        const auto named = node.u16(), numeric = node.u16();
        const auto count = static_cast<std::uint32_t>(named) + numeric;
        require(count <= 4096, Status::limit_exceeded, L"PE 资源条目过多。");
        io::Reader entries(io::slice(resources, static_cast<std::uint64_t>(offset) + 16, static_cast<std::uint64_t>(count) * 8));
        std::optional<std::uint32_t> selected;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto key = entries.u32(), value = entries.u32();
            if (name && key != *name) continue;
            if (!name && selected) continue;
            require(!selected, Status::corrupt, L"PE 资源 ID 重复。");
            require(((value & 0x80000000u) != 0) == directory, Status::corrupt, L"PE 资源层级无效。");
            selected = value & 0x7fffffffu;
        }
        require(selected.has_value(), Status::unsupported, L"未发现受支持的安装器资源。");
        return *selected;
    };
    const auto type_node = child(0, 10, true);
    const auto id_node = child(type_node, id, true);
    const auto leaf = child(id_node, {}, false);
    io::Reader data(io::slice(resources, leaf, 16));
    const auto rva = data.u32(), size = data.u32();
    return resolve(rva, size);
}
} // namespace extract::formats
