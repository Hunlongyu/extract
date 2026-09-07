#pragma once
#include "core/package.h"
#include "io/input.h"

namespace extract::formats {
enum class ArchiveKind { zip, seven_zip };
struct ArchiveLocation { ArchiveKind kind; std::size_t offset; };
std::optional<ArchiveLocation> archive_probe(io::Bytes bytes);
class ArchivePackage final : public Package {
public:
    explicit ArchivePackage(const fs::path& path);
    ~ArchivePackage() override;
    const Catalog& catalog() const override;
    fs::path extract(const fs::path& parent = {}) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
