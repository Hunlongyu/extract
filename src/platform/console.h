#pragma once

#include <string_view>

namespace extract::platform {

// 仅复用调用方的控制台或重定向句柄，不创建控制台窗口。
void write_diagnostic(std::wstring_view message, bool is_error = false) noexcept;

} // namespace extract::platform
