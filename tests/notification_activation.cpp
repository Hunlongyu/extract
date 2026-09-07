#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <cstdio>

// 先运行 Extract.exe --repair-notifications；不自动加入 CTest。
// 此探针只验证冷启动和非法参数，不打开结果目录。
int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    struct Runtime { ~Runtime() { CoUninitialize(); } } runtime;
    const wchar_t* uris[] = {L"hunlongyu-extract://help", L"hunlongyu-extract://job/invalid"};
    const DWORD expected[] = {ERROR_SUCCESS, ERROR_ACCESS_DENIED};
    for (unsigned i = 0; i < 2; ++i) {
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        execute.lpVerb = L"open";
        execute.lpFile = uris[i];
        execute.nShow = SW_HIDE;
        if (!ShellExecuteExW(&execute) || !execute.hProcess) {
            std::printf("Protocol activation failed: %lu\n", GetLastError());
            return 2;
        }
        const auto wait = WaitForSingleObject(execute.hProcess, 10000);
        DWORD code = 0;
        const BOOL read = GetExitCodeProcess(execute.hProcess, &code);
        CloseHandle(execute.hProcess);
        if (wait != WAIT_OBJECT_0 || !read || code != expected[i]) {
            std::printf("Unexpected activation result: %lu\n", code);
            return 3;
        }
    }
    std::puts("PASS: protocol activation starts the exited app and rejects invalid job IDs; no directory opened.");
    return 0;
}
