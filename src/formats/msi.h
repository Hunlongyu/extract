#pragma once
#include "core/package.h"
#include <memory>

namespace extract::formats {
class MsiPackage final : public Package {
public:
    explicit MsiPackage(const fs::path& input);
    ~MsiPackage() override;
    MsiPackage(const MsiPackage&) = delete;
    MsiPackage& operator=(const MsiPackage&) = delete;
    const Catalog& catalog() const override;
    fs::path extract(const fs::path& output_parent = {}) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace extract::formats
