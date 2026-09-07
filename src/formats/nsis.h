#pragma once
#include "core/package.h"
#include "io/input.h"

namespace extract::formats {
bool nsis_probe(io::Bytes input);
bool nsis_uninstaller(io::Bytes input);
class NsisPackage final : public Package {
public:
    explicit NsisPackage(const fs::path& input);
    ~NsisPackage() override;
    const Catalog& catalog() const override;
    fs::path extract(const fs::path& output_parent = {}) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace extract::formats
