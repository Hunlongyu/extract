#pragma once
#include "core/tree.h"
#include "core/progress.h"

namespace extract::platform {
// 每个输入一个本程序子进程；0 表示不设时间上限。
ExtractionResult run_worker(const fs::path& input, const fs::path& output, bool list,
    HANDLE cancel, std::uint64_t timeout_seconds, progress::Observer* observer);
int worker_entry(std::wstring_view request, std::wstring_view response, std::wstring_view cancel);
}
