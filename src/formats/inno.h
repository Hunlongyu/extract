#pragma once
#include "core/package.h"

namespace extract::formats {
class InnoPackage final : public Package {
public:
    explicit InnoPackage(const fs::path& input);
    ~InnoPackage() override;
    const Catalog& catalog() const override;
    fs::path extract(const fs::path& output_parent = {}) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace extract::formats
