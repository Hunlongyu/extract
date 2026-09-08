#pragma once
#include "core/package.h"
#include "io/input.h"

namespace extract::formats {
// EXE 必须有固定封装标记；独立包只接管 *-full.nupkg / *-delta.nupkg。
bool update_package_probe(const fs::path& path, io::Bytes bytes);
class UpdatePackage final : public Package {
public:
    explicit UpdatePackage(const fs::path& path);
    ~UpdatePackage() override;
    const Catalog& catalog() const override;
    fs::path extract(const fs::path& parent = {}) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
