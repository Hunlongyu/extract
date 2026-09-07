#pragma once
#include "core/package.h"
#include "io/input.h"
namespace extract::formats {
struct CabLocation { std::size_t offset, size; };
std::optional<CabLocation> cab_probe(io::Bytes input);
class CabPackage final : public Package {
public:
    explicit CabPackage(const fs::path& path);
    ~CabPackage() override;
    const Catalog& catalog() const override;
    fs::path extract(const fs::path& parent = {}) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
