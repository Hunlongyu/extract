#pragma once
#include "core/types.h"
#include <memory>

namespace extract {
class Package {
public:
    virtual ~Package() = default;
    virtual const Catalog& catalog() const = 0;
    virtual fs::path extract(const fs::path& output_parent = {}) = 0;
};
std::unique_ptr<Package> open_package(const fs::path& input);
void plan_paths(Catalog& catalog);
} // namespace extract
