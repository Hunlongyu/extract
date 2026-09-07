#include "platform/log.h"
#include "platform/files.h"
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 10) return 2;
    extract::log::Options options;
    options.primary = argv[1]; options.fallback = argv[2];
    options.file_bytes = std::stoull(argv[3]); options.directory_bytes = std::stoull(argv[4]);
    options.max_files = std::stoull(argv[5]); options.retention_days = static_cast<unsigned>(std::stoul(argv[6]));
    const auto records = std::stoul(argv[7]);
    const std::wstring_view mode(argv[8]);
    const auto pause = std::stoul(argv[9]);
    extract::log::start(options, L"log-test");
    if (mode == L"runtime-fallback") {
        auto next = extract::log::location();
        const auto star = next.find(L'*');
        if (star == std::wstring::npos) return 3;
        next.replace(star, 1, L"00002");
        if (!CreateDirectoryW(next.c_str(), nullptr)) return 4;
    }
    const extract::fs::path package = options.primary / L"测试安装包.exe";
    const extract::fs::path file = L"app/内容.txt";
    {
        extract::log::Scope scope(L"test.extract", &package, &file);
        extract::log::write(extract::log::Level::info, L"test.escape", L"中文 \"引号\" \\路径\r\n伪造 ERROR\t\u2028");
        extract::log::detail(extract::log::Level::info, L"test.builder", []() -> std::wstring { throw std::bad_alloc(); });
        extract::log::write(extract::log::Level::info, L"test.after_builder", L"continue");
        const std::wstring text(3500, L'测');
        for (unsigned long i = 0; i < records; ++i)
            extract::log::detail(extract::log::Level::info, L"test.record", [&] { return std::to_wstring(i) + L"; " + text; });
        extract::log::write(extract::log::Level::info, L"test.truncate", std::wstring(20000, L'\x01'));
        extract::log::failure(L"test.failure", extract::Failure(extract::Status::corrupt, L"损坏测试，非真实提取错误", 23));
    }
    std::cout << extract::platform::utf8(extract::log::location()) << std::endl;
    if (pause) Sleep(pause);
    extract::log::stop(13);
    return 0;
}
