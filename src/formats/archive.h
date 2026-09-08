#pragma once
#include "core/package.h"
#include "io/input.h"
#include "codecs/stream.h"
#include <functional>

namespace extract::formats {
enum class ArchiveKind { zip, seven_zip };
struct ArchiveLocation { ArchiveKind kind; std::size_t offset; };
struct ZipMember {
    std::uint64_t data_offset, local_header_size;
    codecs::Compression method;
    io::Bytes packed;
};
std::optional<ArchiveLocation> archive_probe(io::Bytes bytes);
class ArchivePackage final : public Package {
public:
    explicit ArchivePackage(const fs::path& path);
    // 调用方持有有界 ZIP 视图，生命周期必须覆盖本对象（PE 资源或磁盘缓存）。
    ArchivePackage(const fs::path& logical_path, io::Bytes zip);
    ~ArchivePackage() override;
    const Catalog& catalog() const override;
    fs::path extract(const fs::path& parent = {}) override;
    void read_member(std::size_t index, const std::function<void(io::Bytes)>& sink) const;
    std::vector<std::byte> read_metadata(std::size_t index, std::size_t limit) const;
    ZipMember zip_member(std::size_t index) const;
    // 每次提供最多 64 KiB，最后以空视图结束；校验在提交输出之前执行。
    void set_block_verifier(std::function<void(std::size_t, std::uint64_t, io::Bytes)> verifier,
                            const std::vector<std::wstring>& algorithms);
    void set_layout(std::wstring format, std::wstring version,
                    const std::vector<fs::path>& paths, std::vector<std::wstring> notes,
                    bool complete = true, const std::vector<std::wstring>& conditions = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
