#pragma once
#include "platform/files.h"

namespace extract::platform {
class JobRecord {
public:
    JobRecord();
    const std::wstring& id() const noexcept { return id_; }
    const fs::path& directory() const noexcept { return directory_; }
    void finish(const std::wstring& summary, const fs::path& open_target = {});
private:
    std::wstring id_;
    fs::path directory_;
    std::vector<Handle> locks_;
};
bool valid_job_id(std::wstring_view id) noexcept;
void open_job(std::wstring_view id);
void open_last_job();
fs::path jobs_directory();
} // namespace extract::platform
