#include "platform/worker.h"
#include "platform/console.h"
#include "platform/log.h"
#include "core/control.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace extract::platform {
namespace {
constexpr DWORD magic = 0x31575845; // EXW1：只在显式继承的匿名管道上传输。
constexpr DWORD frame_limit = 1024 * 1024;
enum class Type : DWORD { request = 1, progress, created, text, details, complete, failure, log_path, committed };
struct Header { DWORD magic_value = magic; DWORD version = 1; Type type; DWORD size; };
using Bytes = std::vector<std::byte>;
struct Writer {
    Bytes bytes;
    void number(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 255));
    }
    void text(std::wstring_view value) {
        number(value.size());
        const auto data = std::as_bytes(std::span(value)); bytes.insert(bytes.end(), data.begin(), data.end());
    }
};
struct Reader {
    std::span<const std::byte> bytes;
    std::uint64_t number() {
        require(bytes.size() >= 8, Status::corrupt, L"工作进程消息截断。");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(bytes[i])) << (8 * i);
        bytes = bytes.subspan(8); return value;
    }
    std::wstring text(std::size_t limit = 32768) {
        const auto size = number();
        require(size <= limit && size <= bytes.size() / sizeof(wchar_t), Status::corrupt, L"工作进程文本长度无效。");
        std::wstring value(static_cast<std::size_t>(size), L'\0');
        std::memcpy(value.data(), bytes.data(), value.size() * sizeof(wchar_t));
        bytes = bytes.subspan(value.size() * sizeof(wchar_t));
        require(value.find(L'\0') == value.npos, Status::corrupt, L"工作进程文本包含空字符。");
        (void)utf8(value); return value;
    }
    void end() { require(bytes.empty(), Status::corrupt, L"工作进程消息存在多余数据。"); }
};
void send(HANDLE pipe, Type type, const Writer& value) {
    require(value.bytes.size() <= frame_limit, Status::internal_error, L"工作进程消息过长。");
    const Header header{magic, 1, type, static_cast<DWORD>(value.bytes.size())};
    write_all(pipe, std::as_bytes(std::span(&header, 1))); write_all(pipe, value.bytes);
}
void send_text(HANDLE pipe, Type type, std::wstring_view text) {
    while (!text.empty()) {
        auto count = (std::min)(text.size(), std::size_t{16384});
        if (count < text.size() && text[count - 1] >= 0xd800 && text[count - 1] <= 0xdbff) --count;
        Writer value; value.text(text.substr(0, count)); send(pipe, type, value); text.remove_prefix(count);
    }
}
struct View {
    void* data;
    explicit View(void* value) : data(value) { if (!data) io_failure(L"无法映射工作进程请求"); }
    ~View() { UnmapViewOfFile(data); }
};
void validate(const Header& header) {
    require(header.magic_value == magic && header.version == 1 && header.size <= frame_limit,
        Status::corrupt, L"工作进程协议版本或长度无效。");
}
HANDLE parse_handle(std::wstring_view text) {
    require(!text.empty() && text.size() <= 20, Status::unsafe_path, L"内部句柄参数无效。");
    std::uintptr_t value = 0;
    for (const auto c : text) {
        require(c >= L'0' && c <= L'9' && value <= ((std::numeric_limits<std::uintptr_t>::max)() - (c - L'0')) / 10,
            Status::unsafe_path, L"内部句柄参数无效。");
        value = value * 10 + (c - L'0');
    }
    require(value && value != (std::numeric_limits<std::uintptr_t>::max)(), Status::unsafe_path, L"内部句柄为空。");
    return reinterpret_cast<HANDLE>(value);
}
struct Owned { fs::path path; BY_HANDLE_FILE_INFORMATION identity{}; FILE_ID_INFO file_id{}; };
bool same_file(const BY_HANDLE_FILE_INFORMATION& a, const BY_HANDLE_FILE_INFORMATION& b,
               const FILE_ID_INFO& x, const FILE_ID_INFO& y) {
    const bool known = std::any_of(std::begin(x.FileId.Identifier), std::end(x.FileId.Identifier), [](BYTE b) { return b != 0; });
    return known && x.VolumeSerialNumber == y.VolumeSerialNumber
        && std::memcmp(x.FileId.Identifier, y.FileId.Identifier, sizeof(x.FileId.Identifier)) == 0
        && a.ftCreationTime.dwHighDateTime == b.ftCreationTime.dwHighDateTime && a.ftCreationTime.dwLowDateTime == b.ftCreationTime.dwLowDateTime
        && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == (b.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}
void cleanup(const std::vector<Owned>& owned) noexcept {
    // 不枚举目录，不按通配符删除；逐项锁定祖先、打开原路径、核对卷/文件 ID/创建时间，再按句柄删除。
    for (auto it = owned.rbegin(); it != owned.rend(); ++it) try {
        auto guards = lock_ancestors(it->path.parent_path());
        Handle file(CreateFileW(extended_path(it->path).c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!file) continue;
        BY_HANDLE_FILE_INFORMATION actual{};
        FILE_ID_INFO actual_id{};
        if (!GetFileInformationByHandle(file.get(), &actual)
            || !GetFileInformationByHandleEx(file.get(), FileIdInfo, &actual_id, sizeof(actual_id))
            || !same_file(actual, it->identity, actual_id, it->file_id)
            || (actual.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            log::write(log::Level::warning, L"worker.cleanup_preserved", it->path.wstring()); continue;
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        const bool removed = SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
        log::detail(removed ? log::Level::info : log::Level::warning, L"worker.cleanup", [&] {
            return it->path.wstring() + (removed ? L"; removed=true" : L"; preserved=true");
        });
    } catch (...) { /* 无法核实归属或目录被替换时保留，不扩大删除范围。 */ }
}
struct Relay final : progress::Observer, control::Observer {
    HANDLE pipe;
    ULONGLONG sent = 0;
    progress::Text package;
    bool extracting = false, full = false;
    explicit Relay(HANDLE value) : pipe(value) {}
    void update(const progress::Snapshot& value) override {
        const bool next_extracting = value.extraction.has_value();
        const bool next_full = next_extracting && value.extraction->total && value.extraction->completed == *value.extraction->total;
        const auto now = GetTickCount64();
        if (now - sent < 50 && package.view() == value.package.view() && extracting == next_extracting && full == next_full) return;
        sent = now; package = value.package; extracting = next_extracting; full = next_full;
        Writer data; data.text(value.package.view()); data.text(value.item.view());
        data.number(static_cast<unsigned>(value.phase)); data.number(value.completed);
        data.number(value.total.has_value()); data.number(value.total.value_or(0));
        data.number(next_extracting);
        data.number(next_extracting ? value.extraction->completed : 0);
        data.number(next_extracting && value.extraction->total.has_value());
        data.number(next_extracting ? value.extraction->total.value_or(0) : 0);
        send(pipe, Type::progress, data);
    }
    void created(const fs::path& path, HANDLE file) override {
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file, &info)) io_failure(L"无法登记临时文件身份");
        FILE_ID_INFO file_id{};
        // 不支持 128 位文件 ID 的文件系统仍可解包；异常后不自动删除无法核实的对象。
        if (!GetFileInformationByHandleEx(file, FileIdInfo, &file_id, sizeof(file_id))) file_id = {};
        Writer data; data.text(path.native()); data.number(info.dwVolumeSerialNumber);
        data.number(info.nFileIndexHigh); data.number(info.nFileIndexLow); data.number(info.dwFileAttributes);
        data.number(info.ftCreationTime.dwHighDateTime); data.number(info.ftCreationTime.dwLowDateTime);
        data.number(file_id.VolumeSerialNumber);
        for (const auto byte : file_id.FileId.Identifier) data.number(byte);
        send(pipe, Type::created, data);
    }
    void committed(const fs::path& path) override { Writer data; data.text(path.native()); send(pipe, Type::committed, data); }
};
std::wstring executable_path() {
    std::wstring path(32768, 0);
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) io_failure(L"无法定位本程序");
    path.resize(size); return path;
}
struct Attributes {
    std::vector<std::byte> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    Attributes() {
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 2, 0, &size); storage.resize(size);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, 2, 0, &size)) io_failure(L"无法初始化工作进程属性");
    }
    ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
};
}

int worker_entry(std::wstring_view request_value, std::wstring_view response_value, std::wstring_view cancel_value) {
    // 安装包字节永远不能选择内部模式；此入口仅接受父进程显式继承的管道和事件句柄。
    Handle request(parse_handle(request_value)), response(parse_handle(response_value)), cancel(parse_handle(cancel_value));
    require(request.get() != response.get() && request.get() != cancel.get() && response.get() != cancel.get(), Status::unsafe_path, L"内部句柄重复。");
    require(GetFileType(response.get()) == FILE_TYPE_PIPE,
        Status::unsafe_path, L"内部工作模式需要管道。");
    require(WaitForSingleObject(cancel.get(), 0) != WAIT_FAILED, Status::unsafe_path, L"内部取消句柄无效。");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG { return EXCEPTION_EXECUTE_HANDLER; });
    try {
        Header header{};
        { View view(MapViewOfFile(request.get(), FILE_MAP_READ, 0, 0, sizeof(header))); std::memcpy(&header, view.data, sizeof(header)); }
        validate(header);
        require(header.type == Type::request, Status::corrupt, L"工作进程缺少请求。");
        Bytes bytes(header.size);
        { View view(MapViewOfFile(request.get(), FILE_MAP_READ, 0, 0, sizeof(header) + header.size));
          std::memcpy(bytes.data(), static_cast<const std::byte*>(view.data) + sizeof(header), bytes.size()); }
        Reader data{bytes};
        const auto input = data.text(), output = data.text(); const auto list = data.number(); data.end();
        require(list <= 1, Status::corrupt, L"工作请求类型无效。");
        request.reset();
        const fs::path input_path(input);
        log::Scope step(L"input.process", &input_path);
        Writer location; location.text(log::location()); send(response.get(), Type::log_path, location);
        Relay relay(response.get()); progress::Connection progress_connection(&relay); control::Connection control_connection(cancel.get(), &relay);
        control::checkpoint();
        auto package = open_package(input_path);
        ExtractionResult result;
        if (list) send_text(response.get(), Type::text, catalog_json(package->catalog(), false));
        else result = extract_tree(std::move(package), fs::path(output));
        send_text(response.get(), Type::details, result.details);
        Writer complete; complete.number(result.complete); complete.text(result.output.native()); complete.text(result.primary_output.native());
        complete.number(result.total_bytes); complete.number(result.file_count); send(response.get(), Type::complete, complete);
        return ERROR_SUCCESS;
    } catch (const Failure& failure) {
        log::failure(L"worker.failed", failure);
        Writer data; data.number(static_cast<unsigned>(failure.status)); data.number(failure.native_code); data.text(failure.message);
        send(response.get(), Type::failure, data); return static_cast<int>(exit_code(failure.status));
    } catch (const std::bad_alloc&) {
        Writer data; data.number(static_cast<unsigned>(Status::internal_error)); data.number(ERROR_NOT_ENOUGH_MEMORY); data.text(L"系统内存或地址空间不足。");
        send(response.get(), Type::failure, data); return ERROR_NOT_ENOUGH_MEMORY;
    } catch (const std::exception& error) {
        log::exception(L"worker.failed", error);
        Writer data; data.number(static_cast<unsigned>(Status::internal_error)); data.number(0); data.text(L"工作进程内部错误，详见日志。");
        send(response.get(), Type::failure, data); return ERROR_UNHANDLED_EXCEPTION;
    }
}

ExtractionResult run_worker(const fs::path& input, const fs::path& output, bool list,
    HANDLE cancel, std::uint64_t timeout_seconds, progress::Observer* observer) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Writer request; request.text(input.native()); request.text(output.native()); request.number(list);
    require(request.bytes.size() <= frame_limit, Status::limit_exceeded, L"工作请求路径过长。");
    Handle request_read(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(Header) + request.bytes.size()), nullptr));
    if (!request_read) io_failure(L"无法创建工作请求映射");
    {
        View view(MapViewOfFile(request_read.get(), FILE_MAP_WRITE, 0, 0, 0));
        const Header header{magic, 1, Type::request, static_cast<DWORD>(request.bytes.size())};
        std::memcpy(view.data, &header, sizeof(header));
        std::memcpy(static_cast<std::byte*>(view.data) + sizeof(header), request.bytes.data(), request.bytes.size());
    }
    HANDLE a = nullptr, b = nullptr;
    if (!CreatePipe(&a, &b, &security, 65536)) io_failure(L"无法创建工作响应管道");
    Handle response_read(a), response_write(b);
    if (!SetHandleInformation(response_read.get(), HANDLE_FLAG_INHERIT, 0)) io_failure(L"无法保护响应管道");
    Handle stop(CreateEventW(&security, TRUE, FALSE, nullptr));
    if (!stop) io_failure(L"无法创建工作进程取消事件");
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) io_failure(L"无法创建工作进程作业");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) io_failure(L"无法配置工作进程回收");
    Attributes attributes;
    HANDLE inherited[]{request_read.get(), response_write.get(), stop.get()};
    if (!UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) io_failure(L"无法限制继承句柄");
    HANDLE jobs[]{job.get()};
    // 创建时原子加入 Job，避免挂起创建后再 Assign 的孤儿进程窗口。
    if (!UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof(jobs), nullptr, nullptr)) io_failure(L"无法绑定工作进程作业");
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes.list;
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW; startup.StartupInfo.wShowWindow = SW_HIDE;
    const auto exe = executable_path();
    auto command = L"\"" + exe + L"\" --internal-worker-v1 " + std::to_wstring(reinterpret_cast<std::uintptr_t>(request_read.get()))
        + L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(response_write.get())) + L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(stop.get()));
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
        nullptr, nullptr, &startup.StartupInfo, &process)) io_failure(L"无法启动本程序的工作进程");
    Handle child(process.hProcess), thread(process.hThread);
    struct Reap { HANDLE job, child; ~Reap() { TerminateJobObject(job, ERROR_PROCESS_ABORTED); WaitForSingleObject(child, 5000); } } reap{job.get(), child.get()};
    request_read.reset(); response_write.reset();
    log::detail(log::Level::info, L"worker.started", [&] { return L"pid=" + std::to_wstring(process.dwProcessId) + L"; input=" + input.wstring(); });
    ExtractionResult result;
    std::vector<Owned> owned;
    std::wstring committed_outputs;
    std::optional<Failure> failure;
    bool finished = false, broken = false;
    std::optional<Status> stopping;
    const auto started = GetTickCount64(); ULONGLONG stop_started = 0;
    Bytes pending;
    const auto consume = [&](const Header& header, std::span<const std::byte> bytes) {
        Reader data{bytes};
        require(!finished, Status::corrupt, L"工作进程在结束消息后继续发送数据。");
        switch (header.type) {
        case Type::progress: {
            progress::Snapshot value; value.package.assign(data.text(512)); value.item.assign(data.text(512));
            const auto phase = data.number(); require(phase <= static_cast<unsigned>(progress::Phase::preparing), Status::corrupt, L"工作进程阶段无效。");
            value.phase = static_cast<progress::Phase>(phase); value.completed = data.number();
            const auto has_total = data.number(), total = data.number(), extracting = data.number(), completed = data.number(), has_extraction_total = data.number(), extraction_total = data.number();
            require(has_total <= 1 && extracting <= 1 && has_extraction_total <= 1 && (!has_total || value.completed <= total)
                && (!has_extraction_total || completed <= extraction_total), Status::corrupt, L"工作进程计量无效。");
            if (has_total) value.total = total;
            if (extracting) value.extraction = progress::Snapshot::Counter{completed, has_extraction_total ? std::optional(extraction_total) : std::nullopt};
            if (observer) observer->update(value);
            break;
        }
        case Type::created: {
            Owned item; item.path = absolute_path(data.text());
            // 只接受临时树或报告临时文件，最终结果不在清理权限内。
            bool temporary = false;
            for (const auto& component : item.path) {
                const auto name = component.wstring();
                if (name.starts_with(L".extract-") && name.ends_with(L".tmp")) temporary = true;
            }
            const auto name = item.path.filename().wstring();
            require(temporary || (name.starts_with(L".report-") && name.ends_with(L".tmp")), Status::unsafe_path, L"工作进程登记了非临时路径。");
            item.identity.dwVolumeSerialNumber = static_cast<DWORD>(data.number()); item.identity.nFileIndexHigh = static_cast<DWORD>(data.number());
            item.identity.nFileIndexLow = static_cast<DWORD>(data.number()); item.identity.dwFileAttributes = static_cast<DWORD>(data.number());
            item.identity.ftCreationTime.dwHighDateTime = static_cast<DWORD>(data.number()); item.identity.ftCreationTime.dwLowDateTime = static_cast<DWORD>(data.number());
            item.file_id.VolumeSerialNumber = data.number();
            for (auto& byte : item.file_id.FileId.Identifier) {
                const auto value = data.number(); require(value <= 255, Status::corrupt, L"临时文件身份数据无效。"); byte = static_cast<BYTE>(value);
            }
            owned.push_back(std::move(item)); break;
        }
        case Type::text: require(list, Status::corrupt, L"非清单任务收到清单数据。"); write_diagnostic(data.text()); break;
        case Type::details: result.details += data.text(); break;
        case Type::log_path: log::write(log::Level::info, L"worker.log", data.text()); break;
        case Type::committed: {
            const auto path = absolute_path(data.text());
            committed_outputs += L"\r\n已提交目录：" + path.wstring();
            log::write(log::Level::info, L"worker.output_committed", path.wstring()); break;
        }
        case Type::complete: {
            const auto complete = data.number(); require(complete <= 1, Status::corrupt, L"工作进程结果无效。");
            result.complete = complete != 0; result.output = data.text(); result.primary_output = data.text(); result.total_bytes = data.number();
            const auto count = data.number(); require(count <= (std::numeric_limits<std::size_t>::max)(), Status::corrupt, L"工作进程文件数溢出。");
            result.file_count = static_cast<std::size_t>(count); finished = true; break;
        }
        case Type::failure: {
            const auto status = data.number(), native = data.number();
            require(status <= static_cast<unsigned>(Status::worker_crashed) && native <= MAXDWORD, Status::corrupt, L"工作进程错误码无效。");
            failure.emplace(static_cast<Status>(status), data.text(), static_cast<DWORD>(native)); finished = true; break;
        }
        default: throw Failure(Status::corrupt, L"工作进程消息类型无效。");
        }
        data.end();
    };
    try {
        for (;;) {
            // 持续排空管道，避免子进程被进度/创建记录写入阻塞；每轮有界以便检查取消。
            for (unsigned reads = 0; reads < 32 && !broken; ++reads) {
                DWORD available = 0;
                if (!PeekNamedPipe(response_read.get(), nullptr, 0, nullptr, &available, nullptr)) {
                    if (GetLastError() != ERROR_BROKEN_PIPE) io_failure(L"工作响应管道失败");
                    broken = true; break;
                }
                if (!available) break;
                const auto start = pending.size(); const auto count = (std::min)(available, DWORD{65536});
                pending.resize(start + count); DWORD read = 0;
                if (!ReadFile(response_read.get(), pending.data() + start, count, &read, nullptr)) io_failure(L"工作响应读取失败");
                pending.resize(start + read);
                std::size_t offset = 0;
                while (pending.size() - offset >= sizeof(Header)) {
                    Header header{}; std::memcpy(&header, pending.data() + offset, sizeof(header)); validate(header);
                    if (pending.size() - offset - sizeof(header) < header.size) break;
                    consume(header, std::span<const std::byte>(pending).subspan(offset + sizeof(header), header.size));
                    offset += sizeof(header) + header.size;
                }
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(offset));
            }
            const auto now = GetTickCount64();
            if (WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0 && broken) break;
            if (!stopping && cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) stopping = Status::cancelled;
            if (!stopping && timeout_seconds && (now - started) / 1000 >= timeout_seconds) stopping = Status::timeout;
            if (stopping && !stop_started) {
                stop_started = now; SetEvent(stop.get());
                log::write(log::Level::warning, L"worker.stopping", *stopping == Status::cancelled ? L"cancelled" : L"timeout");
            }
            if (stop_started && now - stop_started >= 3000 && WaitForSingleObject(child.get(), 0) != WAIT_OBJECT_0)
                if (!TerminateJobObject(job.get(), static_cast<UINT>(exit_code(*stopping)))) io_failure(L"无法停止工作进程");
            WaitForSingleObject(child.get(), 10);
        }
    } catch (...) {
        TerminateJobObject(job.get(), ERROR_PROCESS_ABORTED);
        if (WaitForSingleObject(child.get(), 5000) == WAIT_OBJECT_0) cleanup(owned);
        throw;
    }
    DWORD code = 0; if (!GetExitCodeProcess(child.get(), &code)) io_failure(L"无法读取工作进程退出码");
    log::detail(code == 0 ? log::Level::info : log::Level::warning, L"worker.exited", [&] {
        return L"pid=" + std::to_wstring(process.dwProcessId) + L"; exitCode=" + std::to_wstring(code);
    });
    // 正常路径由 Output 析构清理；异常路径只补清理已登记且身份仍匹配的临时对象。
    if (code || stopping || !finished) cleanup(owned);
    if (stopping) throw Failure(*stopping, std::wstring(*stopping == Status::cancelled ? L"任务已取消，已提交的结果保留。" : L"此安装包超过指定处理时限，已停止；已提交的结果保留。") + committed_outputs);
    if (!finished || !pending.empty()) throw Failure(Status::worker_crashed, L"工作进程异常退出（系统退出码 " + std::to_wstring(code) + L"），已提交的结果保留。" + committed_outputs, code);
    if (failure) { failure->message += committed_outputs; throw *failure; }
    if (code) throw Failure(Status::worker_crashed, L"工作进程提交结果后异常退出（系统退出码 " + std::to_wstring(code) + L"）。", code);
    return result;
}
}
