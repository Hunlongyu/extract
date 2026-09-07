#pragma once
#include "platform/files.h"

namespace extract::io {
class Output {
public:
    Output(const fs::path& parent, std::wstring stem);
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    platform::Handle create_file(const fs::path& relative, bool read_access = false);
    fs::path full_path(const fs::path& relative) const { return staging_ / relative; }
    fs::path commit();
private:
    fs::path parent_;
    fs::path staging_;
    std::wstring stem_;
    std::vector<platform::Handle> ancestors_;
    struct Directory { fs::path path; platform::Handle lock; };
    std::vector<Directory> directories_;
    std::vector<fs::path> files_;
    bool committed_ = false;
    bool cleanup_safe_ = true;
};
} // namespace extract::io
