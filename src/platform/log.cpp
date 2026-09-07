#include "platform/log.h"
#include "platform/files.h"
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace extract::log {
namespace {
thread_local const Scope* context = nullptr;
struct State {
    std::mutex mutex;
    std::atomic_bool active = false;
    Options options;
    fs::path directory, path;
    std::wstring session, prefix;
    platform::Handle file;
    std::vector<platform::Handle> locks;
    std::uint64_t bytes = 0;
    unsigned segment = 0;
    bool fallback = false;
    DWORD directory_error = ERROR_SUCCESS;
    std::wstring directory_reason;
    ULONGLONG started = 0, cleaned = 0, flushed = 0;
};
State state;

std::wstring timestamp(bool filename = false) {
    SYSTEMTIME t{}; GetSystemTime(&t);
    wchar_t text[40]{};
    if (filename) swprintf_s(text, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    else swprintf_s(text, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return text;
}
std::string json_quote(std::wstring_view text) {
    // 限制单字段，保留完整 JSON 和 UTF-8；避免损坏包构造无限长诊断。
    bool shortened = text.size() > 4096;
    if (shortened) {
        text = text.substr(0, 4096);
        if (text.back() >= 0xd800 && text.back() <= 0xdbff) text.remove_suffix(1);
    }
    std::wstring escaped = L"\"";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const wchar_t c : text) {
        if (c == L'"' || c == L'\\') { escaped += L'\\'; escaped += c; }
        else if (c < 32 || c == 0x2028 || c == 0x2029) {
            escaped += L"\\u";
            for (int shift = 12; shift >= 0; shift -= 4) escaped += hex[(c >> shift) & 15];
        } else escaped += c;
    }
    if (shortened) escaped += L"...[truncated]";
    escaped += L'"';
    // 无效 UTF-16 仅在日志中替换，不能让原错误记录消失。
    const int count = WideCharToMultiByte(CP_UTF8, 0, escaped.data(), static_cast<int>(escaped.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<std::size_t>(count), '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, escaped.data(), static_cast<int>(escaped.size()), bytes.data(), count, nullptr, nullptr);
    return bytes;
}
bool owned_name(std::wstring_view name) {
    // extract-YYYYMMDD-HHMMSS-GUID-NNNNN.log；只管理此精确命名的普通文件。
    if (name.size() != 70 || !name.starts_with(L"extract-") || name.substr(66) != L".log") return false;
    for (std::size_t i = 8; i < 23; ++i) {
        if (i == 16) { if (name[i] != L'-') return false; }
        else if (name[i] < L'0' || name[i] > L'9') return false;
    }
    if (name[23] != L'-' || name[60] != L'-') return false;
    for (std::size_t i = 0; i < 36; ++i) {
        const auto c = name[i + 24];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != L'-') return false; }
        else if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F'))) return false;
    }
    for (std::size_t i = 61; i < 66; ++i) if (name[i] < L'0' || name[i] > L'9') return false;
    return true;
}
std::uint64_t ticks(FILETIME time) { return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; }

void cleanup(std::uint64_t reserve = 0) noexcept {
    try {
        struct Item { fs::path path; std::uint64_t size, modified; };
        std::vector<Item> items;
        WIN32_FIND_DATAW data{};
        const auto pattern = platform::extended_path(state.directory / L"extract-*.log");
        const HANDLE search = FindFirstFileW(pattern.c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) return;
        struct Guard { HANDLE value; ~Guard() { FindClose(value); } } guard{search};
        std::uint64_t total = reserve;
        do {
            if ((data.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || !owned_name(data.cFileName)) continue;
            const auto size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            items.push_back({state.directory / data.cFileName, size, ticks(data.ftLastWriteTime)});
            total += size;
        } while (FindNextFileW(search, &data));
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.modified < b.modified; });
        FILETIME now{}; GetSystemTimeAsFileTime(&now);
        const auto age = static_cast<std::uint64_t>(state.options.retention_days) * 86400 * 10000000;
        std::size_t remaining = items.size();
        for (const auto& item : items) {
            const bool expired = ticks(now) > item.modified && ticks(now) - item.modified > age;
            if (!expired && total <= state.options.directory_bytes && remaining <= state.options.max_files) continue;
            // 独占打开后按句柄删除，活动日志的写句柄不共享 DELETE，因此不会被清理。
            platform::Handle handle(CreateFileW(platform::extended_path(item.path).c_str(), DELETE | FILE_READ_ATTRIBUTES,
                0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!handle) continue;
            FILE_ATTRIBUTE_TAG_INFO info{};
            if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &info, sizeof(info)) ||
                (info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) continue;
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
                total -= item.size; --remaining;
            }
        }
        state.cleaned = GetTickCount64();
    } catch (...) {} // 清理失败只会多保留旧日志。
}

void prepare_directory(const fs::path& directory) {
    const auto absolute = platform::absolute_path(directory);
    std::vector<fs::path> missing;
    auto existing = absolute;
    while (GetFileAttributesW(platform::extended_path(existing).c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!existing.has_relative_path()) platform::io_failure(L"日志驱动器不可用");
        missing.push_back(existing); existing = existing.parent_path();
    }
    auto locks = platform::lock_ancestors(existing);
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (!CreateDirectoryW(platform::extended_path(*it).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            platform::io_failure(L"无法创建日志目录");
        locks.push_back(platform::lock_directory(*it));
    }
    state.locks = std::move(locks); state.directory = absolute;
}
void open_segment() {
    state.file.reset();
    require(state.segment < 99999, Status::limit_exceeded, L"日志分段数量超限");
    wchar_t suffix[20]{}; swprintf_s(suffix, L"-%05u.log", ++state.segment);
    state.path = state.directory / (state.prefix + suffix);
    state.file = platform::Handle(CreateFileW(platform::extended_path(state.path).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!state.file) platform::io_failure(L"无法创建日志文件");
    state.bytes = 0;
    cleanup(state.options.file_bytes); // 为当前分段预留空间，限制正在增长的目录。
}
bool switch_directory(const fs::path& directory) noexcept {
    try { state.file.reset(); state.locks.clear(); prepare_directory(directory); open_segment(); return true; }
    catch (const Failure& failure) {
        state.directory_error = failure.native_code ? failure.native_code : static_cast<DWORD>(exit_code(failure.status));
        try { state.directory_reason = failure.message; } catch (...) {}
    } catch (...) { state.directory_error = ERROR_UNHANDLED_EXCEPTION; }
    state.file.reset(); state.locks.clear(); return false;
}
bool append(std::string_view line) noexcept {
    while (!line.empty()) {
        DWORD written = 0;
        if (!WriteFile(state.file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || !written) return false;
        state.bytes += written; line.remove_prefix(written);
    }
    return true;
}
void emergency() noexcept {
    constexpr wchar_t message[] = L"Extract：日志不可写，解包将继续。\r\n";
    OutputDebugStringW(message);
    const auto handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle && handle != INVALID_HANDLE_VALUE) {
        constexpr char bytes[] = "Extract: log unavailable; extraction continues.\r\n";
        DWORD written = 0; WriteFile(handle, bytes, sizeof(bytes) - 1, &written, nullptr);
    }
}
std::string record(Level level, std::wstring_view event, std::wstring_view message) {
    const fs::path* package = nullptr; const fs::path* root = nullptr; const fs::path* file = nullptr;
    for (auto* scope = context; scope; scope = scope->previous) {
        if (!file) file = scope->file;
        if (scope->package) { if (!package) package = scope->package; root = scope->package; }
    }
    const auto full = [](const fs::path* path) {
        if (!path) return std::wstring{};
        std::error_code error;
        const auto absolute = fs::absolute(*path, error);
        return error ? path->wstring() : absolute.lexically_normal().wstring();
    };
    return "{\"time\":" + json_quote(timestamp()) + ",\"level\":" + json_quote(level == Level::error ? L"ERROR" : level == Level::warning ? L"WARN" : L"INFO") +
        ",\"session\":" + json_quote(state.session) + ",\"pid\":" + std::to_string(GetCurrentProcessId()) +
        ",\"tid\":" + std::to_string(GetCurrentThreadId()) + ",\"event\":" + json_quote(event) +
        ",\"stage\":" + json_quote(context ? context->stage : L"session") +
        ",\"rootPackage\":" + json_quote(full(root)) +
        ",\"package\":" + json_quote(full(package)) +
        ",\"file\":" + json_quote(file ? file->wstring() : L"") + ",\"message\":" + json_quote(message) + "}\n";
}
void write_locked(Level level, std::wstring_view event, std::wstring_view message) {
    const auto line = record(level, event, message);
    bool success = true;
    if (state.bytes + line.size() > state.options.file_bytes) {
        if (!FlushFileBuffers(state.file.get())) success = false;
        if (success) { try { open_segment(); } catch (...) { success = false; } }
    }
    if (success) success = append(line);
    const auto now = GetTickCount64();
    if (success && (level == Level::error || now - state.flushed >= 2000)) {
        success = FlushFileBuffers(state.file.get()) != FALSE; state.flushed = now;
    }
    if (!success) {
        const auto error = GetLastError();
        if (!state.fallback && !state.options.fallback.empty()) {
            state.fallback = true;
            if (switch_directory(state.options.fallback)) {
                const auto notice = record(Level::warning, L"log.fallback", L"日志写入中断，切换 AppData；nativeCode=" + std::to_wstring(error));
                success = append(notice) && append(line) && FlushFileBuffers(state.file.get());
            }
        }
        if (!success) { state.active = false; state.file.reset(); state.locks.clear(); emergency(); }
    }
    if (state.active && now - state.cleaned >= 60000) cleanup(state.options.file_bytes - state.bytes);
}
}

void start(const Options& options, std::wstring_view version) noexcept {
    try {
        std::lock_guard guard(state.mutex);
        state.active = false; state.file.reset(); state.locks.clear(); state.path.clear(); state.directory.clear();
        state.options = options; state.segment = 0; state.fallback = false;
        state.directory_error = ERROR_SUCCESS; state.directory_reason.clear();
        require(options.file_bytes >= 256 * 1024 && options.directory_bytes >= options.file_bytes && options.max_files > 0,
            Status::internal_error, L"日志策略无效");
        state.session = platform::unique_id(); state.prefix = L"extract-" + timestamp(true) + L"-" + state.session;
        state.started = state.cleaned = state.flushed = GetTickCount64();
        if (options.primary.empty() || !switch_directory(options.primary)) {
            state.fallback = true;
            if (options.fallback.empty() || !switch_directory(options.fallback)) { emergency(); return; }
        }
        state.active = true;
        write_locked(Level::info, L"session.start", L"version=" + std::wstring(version) + L"; fileLimit=" + std::to_wstring(options.file_bytes) +
            L"; directoryLimit=" + std::to_wstring(options.directory_bytes) + L"; retentionDays=" + std::to_wstring(options.retention_days) +
            L"; maxFiles=" + std::to_wstring(options.max_files));
        if (state.fallback) write_locked(Level::warning, L"log.fallback", L"程序同级日志目录不可用，使用 AppData；primary=" + options.primary.wstring() +
            L"; nativeCode=" + std::to_wstring(state.directory_error) + L"; reason=" + state.directory_reason);
    } catch (...) { state.active = false; state.file.reset(); state.locks.clear(); emergency(); }
}
void start(std::wstring_view version) noexcept {
    try {
        Options options;
        std::vector<wchar_t> path(32768);
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length && length < path.size()) options.primary = fs::path(std::wstring(path.data(), length)).parent_path() / L"log";
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            struct Guard { PWSTR value; ~Guard() { CoTaskMemFree(value); } } guard{local};
            options.fallback = fs::path(local) / L"Extract" / L"log";
        }
        start(options, version);
    } catch (...) { emergency(); }
}
bool enabled() noexcept { return state.active; }
void write(Level level, std::wstring_view event, std::wstring_view message) noexcept {
    if (!state.active) return;
    const auto error = GetLastError();
    try { std::lock_guard guard(state.mutex); if (state.active) write_locked(level, event, message); } catch (...) {}
    SetLastError(error);
}
void failure(std::wstring_view event, const Failure& error) noexcept {
    detail(Level::error, event, [&] {
        wchar_t system[1024]{};
        if (error.native_code) FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
            error.native_code, 0, system, static_cast<DWORD>(std::size(system)), nullptr);
        return L"status=" + std::to_wstring(exit_code(error.status)) + L"; nativeCode=" +
            std::to_wstring(error.native_code) + L"; reason=" + error.message + (system[0] ? L"; system=" + std::wstring(system) : L"");
    });
}
void exception(std::wstring_view event, const std::exception& error) noexcept {
    detail(Level::error, event, [&] {
        const std::string_view reason(error.what());
        const int size = static_cast<int>((std::min)(reason.size(), std::size_t{4096}));
        const int length = MultiByteToWideChar(CP_UTF8, 0, reason.data(), size, nullptr, 0);
        std::wstring message(static_cast<std::size_t>(length), L'\0');
        if (length) MultiByteToWideChar(CP_UTF8, 0, reason.data(), size, message.data(), length);
        return L"内部 C++ 异常；exitCode=574; reason=" + message;
    });
}
void verified(const Entry& entry) noexcept {
    detail(Level::info, L"file.verified", [&] { return entry.path.wstring() + L"; size=" + std::to_wstring(entry.size) + L"; sha256=" + entry.sha256 +
        L"; sourceHashVerified=" + (entry.source_hash_verified ? L"true" : L"false") + L"; msiHashVerified=" + (entry.msi_hash ? L"true" : L"false"); });
}
std::wstring location() noexcept {
    try { std::lock_guard guard(state.mutex); return state.active ? state.directory.wstring() + L"\\" + state.prefix + L"-*.log" : L"日志不可用"; }
    catch (...) { return {}; }
}
void stop(int exit_code) noexcept {
    try {
        std::lock_guard guard(state.mutex);
        if (state.active) {
            write_locked(exit_code == 0 ? Level::info : Level::warning, L"session.end", L"exitCode=" + std::to_wstring(exit_code) +
                L"; elapsedMs=" + std::to_wstring(GetTickCount64() - state.started));
            if (state.file && !FlushFileBuffers(state.file.get())) write_locked(Level::error, L"log.flush_failed", L"结束时刷新日志失败");
            state.file.reset();
            // 写入失败可能已停用日志并释放目录链锁，此时不能再按旧路径清理。
            if (state.active && !state.locks.empty()) cleanup();
        }
        state.active = false; state.file.reset(); state.locks.clear();
    } catch (...) { state.active = false; state.file.reset(); state.locks.clear(); }
}
Scope::Scope(std::wstring_view s, const fs::path* p, const fs::path* f) noexcept
    : previous(context), stage(s), package(p), file(f), exceptions_(std::uncaught_exceptions()), started_(GetTickCount64()) {
    context = this; write(Level::info, L"step.begin");
}
Scope::~Scope() {
    const bool aborted = std::uncaught_exceptions() > exceptions_;
    detail(aborted ? Level::warning : Level::info, aborted ? L"step.aborted" : L"step.end", [&] {
        return L"elapsedMs=" + std::to_wstring(GetTickCount64() - started_);
    });
    context = previous;
}
} // namespace extract::log
