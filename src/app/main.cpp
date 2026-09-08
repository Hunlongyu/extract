#include "extract/version.h"
#include "platform/console.h"
#include "core/package.h"
#include "core/tree.h"
#include "platform/jobs.h"
#include "platform/log.h"
#include "platform/notifications.h"
#include "platform/worker.h"

#include <windows.h>
#include <shellapi.h>
#include <roapi.h>

#include <memory>
#include <string>
#include <string_view>

namespace {

constexpr std::wstring_view usage =
    L"Extract：Windows 安装包解包工具\r\n"
    L"用法：Extract.exe [--list] [--quiet] [--output <父目录>] [--timeout <秒>] [--] <安装包路径...>\r\n"
    L"支持 MSI 内嵌/外置多 CAB 与松散文件、标准 CAB、WiX Burn、ZIP/7z 及其 PE 自解压外壳、NSIS Unicode 3.x／部分 ANSI 2.x、受支持版本的 Inno Setup、Velopack/Squirrel 完整离线包，以及未加密 MSIX/APPX/Bundle。\r\n"
    L"提取时自动展开识别到的内嵌安装包；内层未完成则报告部分完成。\r\n"
    L"--list 只列出一个包的清单，已验证的编译器版本见 README。\r\n"
    L"--timeout 为每个输入包的时限，0 表示关闭（默认）；取消会停止整个批次。\r\n"
    L"其它命令：--cancel <任务 GUID>、--cancel-last、--help、--version、--open-last-result、--repair-notifications、--unregister-notifications。\r\n";

int run() {
    extract::log::Scope run_step(L"application.run");
    int count = 0;
    const std::unique_ptr<wchar_t*, decltype(&LocalFree)> arguments(
        CommandLineToArgvW(GetCommandLineW(), &count), &LocalFree);
    if (!arguments) {
        const DWORD error = GetLastError();
        extract::platform::write_diagnostic(L"无法读取启动参数。\r\n", true);
        return static_cast<int>(error != ERROR_SUCCESS ? error : ERROR_INVALID_PARAMETER);
    }

    if (count < 2) {
        extract::platform::write_diagnostic(usage, true);
        (void)extract::platform::show_notification(L"Extract", L"请将安装包或 ZIP/7z 归档拖到程序图标上。", {});
        return ERROR_BAD_ARGUMENTS;
    }

    const std::wstring_view first(arguments.get()[1]);
    if (count == 5 && first == L"--internal-worker-v1")
        return extract::platform::worker_entry(arguments.get()[2], arguments.get()[3], arguments.get()[4]);
    if (count == 3 && first == L"--cancel") { extract::platform::cancel_job(arguments.get()[2]); return ERROR_SUCCESS; }
    if (count == 2 && first == L"--cancel-last") { extract::platform::cancel_last_job(); return ERROR_SUCCESS; }
    if (count == 3 && first == L"--open-notification") {
        extract::platform::activate_notification(arguments.get()[2]);
        return ERROR_SUCCESS;
    }
    if (count == 2 && first == L"--repair-notifications") { extract::platform::register_notifications(); return ERROR_SUCCESS; }
    if (count == 2 && first == L"--unregister-notifications") { extract::platform::unregister_notifications(); return ERROR_SUCCESS; }
    if (count == 2 && first == L"--open-last-result") { extract::platform::open_last_job(); return ERROR_SUCCESS; }
    if (count == 2 && first == L"--help") {
        extract::platform::write_diagnostic(usage);
        return ERROR_SUCCESS;
    }
    if (count == 2 && first == L"--version") {
        const std::wstring message = L"Extract " + std::wstring(extract::version) + L" (MSI / CAB / Burn / Inno Setup / NSIS / Velopack / Squirrel / MSIX / APPX / Bundle / ZIP / 7z)\r\n";
        extract::platform::write_diagnostic(message);
        return ERROR_SUCCESS;
    }

    bool accept_options = true;
    bool list_only = false;
    bool quiet = false;
    bool output_specified = false;
    bool timeout_specified = false;
    std::uint64_t timeout_seconds = 0;
    extract::fs::path output_parent;
    std::vector<extract::fs::path> inputs;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view argument(arguments.get()[index]);
        if (accept_options && argument == L"--") {
            accept_options = false;
            continue;
        }
        if (accept_options && argument == L"--list") { list_only = true; continue; }
        if (accept_options && argument == L"--quiet") { quiet = true; continue; }
        if (accept_options && argument == L"--timeout") {
            if (++index >= count || timeout_specified) { extract::platform::write_diagnostic(L"--timeout 需要非负整数秒，且只能指定一次。\r\n", true); return ERROR_BAD_ARGUMENTS; }
            const std::wstring_view value(arguments.get()[index]);
            if (value.empty()) return ERROR_BAD_ARGUMENTS;
            for (auto c : value) {
                if (c < L'0' || c > L'9' || timeout_seconds > (MAXDWORD - static_cast<unsigned>(c - L'0')) / 10) return ERROR_BAD_ARGUMENTS;
                timeout_seconds = timeout_seconds * 10 + (c - L'0');
            }
            timeout_specified = true; continue;
        }
        if (accept_options && argument == L"--output") {
            if (++index >= count || output_specified || arguments.get()[index][0] == L'\0') {
                extract::platform::write_diagnostic(L"--output 需要一个输出父目录，且只能指定一次。\r\n", true);
                return ERROR_BAD_ARGUMENTS;
            }
            output_parent = arguments.get()[index];
            output_specified = true;
            continue;
        }
        if (argument.empty() || (accept_options && argument.starts_with(L"-"))) {
            extract::platform::write_diagnostic(L"参数无效；以连字符开头的文件名请放在 -- 后。\r\n", true);
            return ERROR_BAD_ARGUMENTS;
        }
        inputs.emplace_back(argument);
    }
    if (inputs.empty() || (list_only && (inputs.size() != 1 || output_specified))) {
        extract::platform::write_diagnostic(usage, true);
        return ERROR_BAD_ARGUMENTS;
    }
    extract::log::detail(extract::log::Level::info, L"batch.begin", [&] {
        return L"inputs=" + std::to_wstring(inputs.size()) + L"; listOnly=" + (list_only ? L"true" : L"false") +
            L"; quiet=" + (quiet ? L"true" : L"false") + L"; outputParent=" + output_parent.wstring();
    });

    std::optional<extract::platform::JobRecord> record;
    extract::platform::recover_last_job();
    if (!list_only) record.emplace();
    const bool report_progress = record.has_value() && !quiet;
    extract::platform::ProgressNotification progress_notification(report_progress ? std::wstring_view(record->id()) : std::wstring_view{});
    extract::progress::Connection progress_connection(report_progress ? &progress_notification : nullptr);
    std::size_t input_index = 0;
    std::wstring summary;
    std::wstring failure_reason;
    extract::fs::path single_output;
    int first_error = ERROR_SUCCESS;
    std::size_t succeeded = 0;
    std::size_t partial = 0;
    bool cancelled = false;
    std::size_t cancelled_inputs = 0;
    for (const auto& input : inputs) {
        if (record && WaitForSingleObject(record->cancel_event(), 0) == WAIT_OBJECT_0) { cancelled = true; break; }
        progress_notification.batch(++input_index, inputs.size());
        extract::progress::Scope input_progress(extract::progress::Phase::analyzing, {}, {}, input.native());
        extract::log::Scope input_step(L"input.process", &input);
        try {
            if (record) try { record->update(summary + L"正在处理：" + input.wstring() + L"\r\n"); }
                catch (const extract::Failure& failure) { extract::log::failure(L"job.save_failed", failure); }
            const auto result = extract::platform::run_worker(input, output_parent, list_only,
                record ? record->cancel_event() : nullptr, timeout_seconds, report_progress ? &progress_notification : nullptr);
            bool complete = true;
            if (!list_only) {
                complete = result.complete;
                single_output = result.primary_output;
                const auto status = complete ? L"文件提取完成：" : L"部分完成，仍有路径、载荷或内层内容未还原：";
                extract::platform::write_diagnostic(status + result.primary_output.wstring() + L"\r\n" + result.details);
                summary += status + input.wstring() + L"\r\n输出：" + result.primary_output.wstring() + L"\r\n" + result.details + L"\r\n";
            }
            if (complete) ++succeeded;
            else { ++partial; if (first_error == ERROR_SUCCESS) first_error = ERROR_PARTIAL_COPY; }
            extract::log::write(complete ? extract::log::Level::info : extract::log::Level::warning,
                L"input.result", list_only ? L"listed" : complete ? L"complete" : L"partial");
        } catch (const extract::Failure& failure) {
            if (failure.status == extract::Status::cancelled) { cancelled = true; ++cancelled_inputs; }
            failure_reason = failure.message;
            extract::log::failure(L"input.failed", failure);
            extract::platform::write_diagnostic(input.wstring() + L"：" + failure.message + L"\r\n", true);
            summary += std::wstring(cancelled ? L"已取消：" : L"失败：") + input.wstring() + L"\r\n原因：" + failure.message + L"\r\n\r\n";
            if (first_error == ERROR_SUCCESS) first_error = extract::exit_code(failure.status);
        } catch (const std::bad_alloc& error) {
            failure_reason = L"系统内存或地址空间不足。";
            extract::log::exception(L"input.out_of_memory", error);
            const auto message = input.wstring() + L"：系统内存或地址空间不足；32 位程序可改用 x64 或 ARM64 版本。\r\n";
            extract::platform::write_diagnostic(message, true);
            summary += L"失败：" + message + L"\r\n";
            if (first_error == ERROR_SUCCESS) first_error = ERROR_NOT_ENOUGH_MEMORY;
        } catch (const std::exception& error) {
            failure_reason = L"内部错误，点击查看任务记录。";
            extract::log::exception(L"input.failed", error);
            extract::platform::write_diagnostic(input.wstring() + L"：内部错误。\r\n", true);
            summary += L"失败：" + input.wstring() + L"\r\n原因：内部错误。\r\n\r\n";
            if (first_error == ERROR_SUCCESS) first_error = ERROR_UNHANDLED_EXCEPTION;
        }
        if (record) try { record->update(summary); }
            catch (const extract::Failure& failure) { extract::log::failure(L"job.save_failed", failure); }
        if (cancelled) break;
    }
    if (cancelled) {
        summary += L"批次已取消；未开始 " + std::to_wstring(inputs.size() - input_index) + L" 个安装包。已提交的结果保留。\r\n";
        if (failure_reason.empty()) failure_reason = L"批次已取消。";
    }
    if (record) {
        const auto target = inputs.size() == 1 && succeeded == 1 ? single_output : record->directory();
        // 结果日志或通知失败不能把已经成功提交的载荷改成解包失败。
        try { record->finish(summary, target); }
        catch (const extract::Failure& failure) {
            extract::log::failure(L"job.save_failed", failure);
            extract::platform::write_diagnostic(failure.message + L"\r\n", true);
        }
        if (!quiet) {
            std::wstring title, body;
            if (inputs.size() == 1) {
                title = std::wstring(cancelled ? L"已取消 · " : succeeded == 1 ? L"已完成 · " : partial == 1 ? L"部分完成 · " : L"失败 · ")
                    + inputs.front().filename().wstring();
                if (succeeded == 1) body = L"输出：" + single_output.wstring();
                else if (partial == 1) body = L"已保留部分文件。输出：" + single_output.wstring();
                else body = L"原因：" + failure_reason;
            } else {
                title = cancelled ? L"批量任务已取消" : first_error == ERROR_SUCCESS ? L"批量处理已完成" : L"批量处理存在未完成项";
                body = L"完成 " + std::to_wstring(succeeded) + L" 个，部分完成 " + std::to_wstring(partial)
                    + L" 个，失败 " + std::to_wstring(input_index - succeeded - partial - cancelled_inputs) + L" 个。\n"
                    + inputs[0].filename().wstring() + L"、" + inputs[1].filename().wstring()
                    + (inputs.size() > 2 ? L" 等 " + std::to_wstring(inputs.size()) + L" 个安装包。" : L"。");
                if (cancelled) body += L"\n取消 " + std::to_wstring(cancelled_inputs) + L" 个，未开始 " + std::to_wstring(inputs.size() - input_index) + L" 个。";
            }
            const auto action_label = inputs.size() == 1 && succeeded == 1 ? L"打开文件夹" : L"查看结果";
            const HRESULT notified = progress_notification.complete(title, body, record->id(), succeeded == inputs.size(), action_label);
            const auto notification_status = notified == S_OK ? L"通知已提交；显示取决于系统设置。" :
                (notified == S_FALSE ? L"系统通知已禁用。" : L"系统通知提交失败。");
            extract::log::detail(notified == S_OK ? extract::log::Level::info : extract::log::Level::warning, L"notification.result", [&] {
                return std::wstring(notification_status) + L" HRESULT=" + std::to_wstring(static_cast<unsigned long>(notified));
            });
            try { record->finish(summary + notification_status + L" HRESULT=" + std::to_wstring(static_cast<unsigned long>(notified)) + L"\r\n", target); } catch (...) {}
            if (notified != S_OK) extract::platform::write_diagnostic(std::wstring(notification_status) + L"\r\n", true);
        }
    }
    extract::log::detail(extract::log::Level::info, L"batch.end", [&] {
        return L"complete=" + std::to_wstring(succeeded) + L"; partial=" + std::to_wstring(partial) +
            L"; failed=" + std::to_wstring(input_index - succeeded - partial - cancelled_inputs)
            + L"; cancelled=" + std::to_wstring(cancelled_inputs) + L"; notStarted=" + std::to_wstring(inputs.size() - input_index);
    });
    if (cancelled) return ERROR_CANCELLED;
    return partial != 0 || (first_error != ERROR_SUCCESS && succeeded != 0) ? ERROR_PARTIAL_COPY : first_error;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    extract::log::start(extract::version);
    int result = ERROR_UNHANDLED_EXCEPTION;
    struct LogGuard { int& result; ~LogGuard() { extract::log::stop(result); } } log_guard{result};
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initialized)) {
        extract::log::detail(extract::log::Level::error, L"runtime.initialize_failed", [&] {
            return L"HRESULT=" + std::to_wstring(static_cast<unsigned long>(initialized));
        });
        return result;
    }
    struct RuntimeGuard { ~RuntimeGuard() { RoUninitialize(); } } runtime_guard;
    try {
        result = run();
    } catch (const extract::Failure& failure) {
        extract::log::failure(L"application.failed", failure);
        extract::platform::write_diagnostic(failure.message + L"\r\n", true);
        result = extract::exit_code(failure.status);
    } catch (const std::exception& error) {
        extract::log::exception(L"application.failed", error);
        extract::platform::write_diagnostic(L"程序发生内部错误。\r\n", true);
    } catch (...) {
        extract::log::write(extract::log::Level::error, L"application.failed", L"内部异常；exitCode=574");
        extract::platform::write_diagnostic(L"程序发生内部错误。\r\n", true);
    }
    return result;
}
