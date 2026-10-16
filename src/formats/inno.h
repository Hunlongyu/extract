#pragma once
#include "core/package.h"
#include "io/input.h"

namespace extract::formats {
bool inno_probe(io::Bytes input);
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
