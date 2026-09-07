#include "core/progress.h"
#include <windows.h>
#include <algorithm>
#include <limits>

namespace extract::progress {
namespace {
thread_local Observer* observer = nullptr;
thread_local Scope* current = nullptr;
thread_local bool reporting = false;
}
void Text::assign(std::wstring_view value) noexcept {
    size = (std::min)(value.size(), bytes.size());
    if (size && size < value.size() && value[size - 1] >= 0xd800 && value[size - 1] <= 0xdbff) --size;
    std::copy_n(value.data(), size, bytes.data());
}
Connection::Connection(Observer* value) noexcept : previous_(observer) { observer = value; }
Connection::~Connection() { observer = previous_; }
Scope::Scope(Phase phase, std::optional<std::uint64_t> total, std::wstring_view item,
             std::wstring_view package) noexcept : previous_(current) {
    if (previous_) value_.package = previous_->value_.package;
    if (!package.empty()) value_.package.assign(package);
    value_.phase = phase; value_.total = total; value_.item.assign(item);
    current = this;
    pulse();
}
Scope::~Scope() { current = previous_; pulse(); }
void pulse() noexcept {
    if (!current || !observer || reporting) return;
    const auto error = GetLastError();
    reporting = true;
    try { observer->update(current->value_); }
    catch (...) { /* 通知不可用或观察器失败，不改变解包结果。 */ }
    reporting = false;
    SetLastError(error);
}
void advance(std::uint64_t bytes) noexcept {
    if (!current) return;
    auto& value = current->value_;
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    value.completed += (std::min)(bytes, maximum - value.completed);
    // 声明总量不可靠时改为不确定进度，不伪装成完整的 100%。
    if (value.total && value.completed > *value.total) value.total.reset();
    pulse();
}
void file(std::wstring_view name) noexcept {
    if (current) { current->value_.item.assign(name); pulse(); }
}
std::wstring_view phase_name(Phase phase) noexcept {
    switch (phase) {
    case Phase::analyzing: return L"正在分析安装包";
    case Phase::extracting: return L"正在提取文件";
    case Phase::decoding: return L"正在解压数据块";
    case Phase::verifying: return L"正在校验文件";
    case Phase::scanning: return L"正在检查内层安装包";
    case Phase::finalizing: return L"正在整理结果";
    case Phase::preparing: return L"正在准备解包数据";
    }
    return L"正在处理";
}
} // namespace extract::progress
