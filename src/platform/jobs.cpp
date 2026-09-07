#include "platform/jobs.h"
#include "platform/log.h"
#include <shlobj.h>
#include <shellapi.h>

namespace extract::platform {
namespace {
fs::path local_data() {
    PWSTR value = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value);
    if (FAILED(result)) throw Failure(Status::io_error, L"无法定位本地任务记录目录。");
    struct Guard { PWSTR p; ~Guard() { CoTaskMemFree(p); } } guard{value};
    return absolute_path(value);
}
void replace_text(const fs::path& directory, const wchar_t* name, const std::wstring& text) {
    const auto temporary = directory / (L"." + unique_id() + L".tmp");
    Handle file(CreateFileW(extended_path(temporary).c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) io_failure(L"无法创建任务记录");
    try {
        const auto bytes = utf8(text);
        write_all(file.get(), std::as_bytes(std::span(bytes)));
        if (!FlushFileBuffers(file.get())) io_failure(L"无法保存任务记录");
        const DWORD renamed = rename_in_place(file.get(), name, true);
        if (renamed != ERROR_SUCCESS) io_failure(L"无法提交任务记录：" + std::wstring(name), renamed);
    } catch (...) {
        file.reset();
        DeleteFileW(extended_path(temporary).c_str());
        throw;
    }
}
std::wstring read_text(const fs::path& path) {
    Handle file(CreateFileW(extended_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file) io_failure(L"无法打开任务记录");
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes))) io_failure(L"无法检查任务记录");
    require((attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0,
            Status::unsafe_path, L"任务记录不能是链接或目录。");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) io_failure(L"无法读取任务记录长度");
    require(size.QuadPart > 0 && size.QuadPart < 65536, Status::corrupt, L"任务记录长度无效。");
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) io_failure(L"任务记录读取不完整");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    require(length > 0, Status::corrupt, L"任务记录编码无效。");
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), text.data(), length) != length)
        io_failure(L"任务记录解码失败");
    return text;
}
}

fs::path jobs_directory() { return local_data() / L"Extract" / L"jobs"; }

JobRecord::JobRecord() : id_(unique_id()) {
    auto path = local_data();
    locks_ = lock_ancestors(path);
    for (const auto& component : {std::wstring(L"Extract"), std::wstring(L"jobs"), id_}) {
        path /= component;
        if (!CreateDirectoryW(extended_path(path).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            io_failure(L"无法创建任务记录目录");
        locks_.push_back(lock_directory(path));
    }
    directory_ = path;
    log::detail(log::Level::info, L"job.created", [&] { return L"job=" + id_ + L"; directory=" + directory_.wstring(); });
    finish(L"任务正在执行。\r\n");
    replace_text(directory_.parent_path(), L"latest.txt", id_);
}

void JobRecord::finish(const std::wstring& summary, const fs::path& open_target) {
    replace_text(directory_, L"summary.txt", summary + L"\r\n详细日志：" + log::location() + L"\r\n");
    replace_text(directory_, L"open-target.txt", (open_target.empty() ? directory_ : open_target).wstring());
}

bool valid_job_id(std::wstring_view id) noexcept {
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const wchar_t c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != L'-') return false; }
        else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) return false;
    }
    return true;
}

void open_job(std::wstring_view id) {
    require(valid_job_id(id), Status::unsafe_path, L"通知任务标识无效。");
    const auto directory = jobs_directory() / id;
    auto guards = lock_ancestors(directory);
    const auto target = absolute_path(read_text(directory / L"open-target.txt"));
    auto target_guards = lock_ancestors(target);
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) io_failure(L"无法打开结果目录", static_cast<DWORD>(result));
}

void open_last_job() {
    const auto directory = jobs_directory();
    auto guards = lock_ancestors(directory);
    open_job(read_text(directory / L"latest.txt"));
}
} // namespace extract::platform
