#pragma once
#include "core/tree.h"

namespace extract {
struct LayoutLayer {
    Catalog catalog;
    fs::path directory;
};
ExtractionResult compact_output(std::vector<LayoutLayer>& layers, const fs::path& input,
    const fs::path& parent, bool complete);
}
