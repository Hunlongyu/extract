#pragma once
#include "core/package.h"

namespace extract::formats {
bool msix_extension(const fs::path& path);
bool msix_catalog(const Catalog& catalog);
class MsixPackage final : public Package {
public:
    explicit MsixPackage(const fs::path& path);
    ~MsixPackage() override;
    const Catalog& catalog() const override;
    fs::path extract(const fs::path& parent = {}) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
