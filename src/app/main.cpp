#include "extract/version.h"
#include "platform/console.h"

#include <windows.h>
#include <shellapi.h>

#include <memory>
#include <string>
#include <string_view>

namespace {

constexpr std::wstring_view usage =
    L"Extract：Windows 安装包解包工具（工程初始化阶段）\r\n"
    L"用法：Extract.exe --help | --version | [--] <安装包路径...>\r\n"
    L"当前尚未实现格式解析、文件提取和系统通知。\r\n";

int run() {
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
        return ERROR_BAD_ARGUMENTS;
    }

    const std::wstring_view first(arguments.get()[1]);
    if (count == 2 && first == L"--help") {
        extract::platform::write_diagnostic(usage);
        return ERROR_SUCCESS;
    }
    if (count == 2 && first == L"--version") {
        const std::wstring message = L"Extract " + std::wstring(extract::version) + L" (project skeleton)\r\n";
        extract::platform::write_diagnostic(message);
        return ERROR_SUCCESS;
    }

    bool accept_options = true;
    bool has_input = false;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view argument(arguments.get()[index]);
        if (accept_options && argument == L"--") {
            accept_options = false;
            continue;
        }
        if (argument.empty() || (accept_options && argument.starts_with(L"-"))) {
            extract::platform::write_diagnostic(L"参数无效；以连字符开头的文件名请放在 -- 后。\r\n", true);
            return ERROR_BAD_ARGUMENTS;
        }
        has_input = true;
    }
    if (!has_input) {
        extract::platform::write_diagnostic(usage, true);
        return ERROR_BAD_ARGUMENTS;
    }

    // 初始化阶段只建立宽字符入口，不读取或执行输入文件，也不报告解包成功。
    extract::platform::write_diagnostic(L"当前为工程骨架，尚未实现安装包解包；未生成输出文件。\r\n", true);
    return ERROR_NOT_SUPPORTED;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        return run();
    } catch (...) {
        extract::platform::write_diagnostic(L"程序发生内部错误。\r\n", true);
        return ERROR_UNHANDLED_EXCEPTION;
    }
}
