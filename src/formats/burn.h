#pragma once
#include "core/package.h"
#include "io/input.h"
namespace extract::formats {
bool burn_probe(io::Bytes bytes);
class BurnPackage final : public Package {
public:
    explicit BurnPackage(const fs::path& path);
    ~BurnPackage() override;
    const Catalog& catalog() const override;
    fs::path extract(const fs::path& parent = {}) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
