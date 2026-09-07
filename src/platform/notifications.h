#pragma once
#include <windows.h>
#include <string_view>

namespace extract::platform {
void register_notifications();
void unregister_notifications();
HRESULT show_notification(std::wstring_view title, std::wstring_view body, std::wstring_view job_id) noexcept;
void activate_notification(std::wstring_view uri);
} // namespace extract::platform
