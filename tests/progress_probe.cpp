#include "core/progress.h"
#include "core/package.h"
#include "core/tree.h"
#include "platform/files.h"
#include <cstdio>
#include <thread>

using namespace extract;
namespace {
std::wstring json_string(std::wstring_view value) {
    std::wstring result = L"\"";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const auto c : value) {
        if (c == L'"' || c == L'\\') { result += L'\\'; result += c; }
        else if (c < 0x20) { result += L"\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += c;
    }
    return result + L'"';
}
struct Trace : progress::Observer {
    bool throwing = false;
    void update(const progress::Snapshot& value) override {
        if (throwing) throw Failure(Status::internal_error, L"模拟进度观察器失败");
        const auto text = L"{\"phase\":" + std::to_wstring(static_cast<int>(value.phase))
            + L",\"package\":" + json_string(value.package.view()) + L",\"item\":" + json_string(value.item.view())
            + L",\"done\":" + std::to_wstring(value.completed) + L",\"total\":"
            + (value.total ? std::to_wstring(*value.total) : L"null") + L"}";
        std::printf("%s\n", platform::utf8(text).c_str());
    }
};
struct Capture : progress::Observer {
    progress::Snapshot last;
    unsigned count = 0;
    void update(const progress::Snapshot& value) override { last = value; ++count; SetLastError(ERROR_ACCESS_DENIED); progress::pulse(); }
};
void self_test() {
    Capture capture;
    progress::Connection connection(&capture);
    progress::Scope parent(progress::Phase::extracting, 100, L"a.bin", L"parent.zip");
    SetLastError(ERROR_DISK_FULL);
    progress::advance(30);
    require(GetLastError() == ERROR_DISK_FULL, Status::internal_error, L"进度回调覆盖了系统错误码。");
    {
        progress::Scope child(progress::Phase::verifying, 200, L"child.bin");
        progress::advance(150);
        require(capture.last.completed == 150 && capture.last.total == 200 && capture.last.package.view() == L"parent.zip",
                Status::internal_error, L"子阶段计数或包名错误。");
    }
    require(capture.last.completed == 30 && capture.last.total == 100 && capture.last.item.view() == L"a.bin",
            Status::internal_error, L"子阶段污染了提取计数。");
    const auto count = capture.count;
    std::thread worker([] { progress::Scope stage(progress::Phase::extracting, 20); progress::advance(20); });
    worker.join();
    require(capture.count == count, Status::internal_error, L"线程之间的进度串扰。");
    { progress::Connection disabled(nullptr); progress::advance(10); }
    require(capture.count == count, Status::internal_error, L"禁用观察器后仍有通知。");
    progress::advance(5);
    require(capture.last.completed == 45, Status::internal_error, L"恢复观察器后计数丢失。");
    progress::advance((std::numeric_limits<std::uint64_t>::max)());
    require(!capture.last.total && capture.last.completed == (std::numeric_limits<std::uint64_t>::max)(),
            Status::internal_error, L"进度溢出或仍显示不可靠的总量。");
    progress::Text text;
    text.assign(std::wstring(511, L'a') + L"\xd83d\xde00");
    require(text.size == 511, Status::internal_error, L"截断了 Unicode 代理对。");
}
}
int wmain(int count, wchar_t** arguments) {
    try {
        if (count == 2 && std::wstring_view(arguments[1]) == L"--self-test") { self_test(); return 0; }
        if (count < 3 || count > 4) return 2;
        Trace trace; trace.throwing = count == 4 && std::wstring_view(arguments[3]) == L"--throw";
        progress::Connection connection(&trace);
        const fs::path input(arguments[1]);
        progress::Scope stage(progress::Phase::analyzing, {}, {}, input.native());
        const auto result = extract_tree(open_package(input), arguments[2]);
        return result.complete ? 0 : ERROR_PARTIAL_COPY;
    } catch (const Failure& failure) { std::fprintf(stderr, "%s\n", platform::utf8(failure.message).c_str()); return exit_code(failure.status); }
    catch (...) { return 99; }
}
