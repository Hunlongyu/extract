#include "platform/console.h"

#include <windows.h>

#include <limits>
#include <string>

namespace extract::platform {

void write_diagnostic(const std::wstring_view message, const bool is_error) noexcept {
    try {
        if (message.empty() || message.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return;
        }

        const std::wstring terminated(message);
        OutputDebugStringW(terminated.c_str());

        const DWORD stream = is_error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE;
        HANDLE output = GetStdHandle(stream);
        if (output == nullptr || output == INVALID_HANDLE_VALUE) {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
                return;
            }
            output = GetStdHandle(stream);
        }
        if (output == nullptr || output == INVALID_HANDLE_VALUE) {
            return;
        }

        DWORD mode = 0;
        if (GetConsoleMode(output, &mode)) {
            std::size_t offset = 0;
            while (offset < message.size()) {
                DWORD written = 0;
                if (!WriteConsoleW(output, message.data() + offset,
                                   static_cast<DWORD>(message.size() - offset), &written, nullptr)
                    || written == 0) {
                    return;
                }
                offset += written;
            }
            return;
        }

        const int length = static_cast<int>(message.size());
        const int byte_count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                                  message.data(), length, nullptr, 0, nullptr, nullptr);
        if (byte_count <= 0) {
            return;
        }
        std::string bytes(static_cast<std::size_t>(byte_count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, message.data(), length,
                                bytes.data(), byte_count, nullptr, nullptr) != byte_count) {
            return;
        }
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            if (!WriteFile(output, bytes.data() + offset,
                           static_cast<DWORD>(bytes.size() - offset), &written, nullptr)
                || written == 0) {
                return;
            }
            offset += written;
        }
    } catch (...) {
        // 诊断输出失败不能改变主任务的退出状态。
    }
}

} // namespace extract::platform
