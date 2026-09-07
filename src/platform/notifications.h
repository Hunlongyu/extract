#pragma once
#include <windows.h>
#include <string_view>
#include <memory>
#include "core/progress.h"

namespace extract::platform {
void register_notifications();
void unregister_notifications();
HRESULT show_notification(std::wstring_view title, std::wstring_view body, std::wstring_view job_id, bool suppress_popup = false) noexcept;
void activate_notification(std::wstring_view uri);
class ProgressNotification final : public progress::Observer {
public:
    explicit ProgressNotification(std::wstring_view job_id) noexcept;
    ~ProgressNotification();
    void batch(std::size_t index, std::size_t count) noexcept;
    void update(const progress::Snapshot& value) noexcept override;
    void close() noexcept;
    HRESULT complete(std::wstring_view title, std::wstring_view body, std::wstring_view job_id) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace extract::platform
