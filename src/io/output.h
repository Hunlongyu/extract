#pragma once
#include "platform/files.h"

namespace extract::io {
class Staging;
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
    friend class Staging;
    fs::path parent_;
    fs::path staging_;
    std::wstring stem_;
    std::vector<platform::Handle> ancestors_;
    struct Directory { fs::path path; platform::Handle lock; FILE_ID_INFO identity{}; FILETIME created{}; };
    std::vector<Directory> directories_;
    struct File { fs::path path; FILE_ID_INFO identity{}; FILETIME created{}; };
    std::vector<File> files_;
    bool committed_ = false;
    bool cleanup_safe_ = true;
};

// Keep each successfully decoded layer locked and uncommitted until its final
// layout has been written. The worker still tracks every temporary file by ID.
class Staging {
public:
    Staging();
    ~Staging();
    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;
    void finish() noexcept;
    static bool active() noexcept;
    static void updated(const fs::path& path, HANDLE file);
private:
    friend class Output;
    fs::path capture(Output& output);
    struct Saved {
        std::vector<platform::Handle> ancestors;
        std::vector<Output::Directory> directories;
        std::vector<Output::File> files;
    };
    std::vector<Saved> saved_;
};
} // namespace extract::io
