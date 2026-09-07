#include "io/output.h"
#include "core/package.h"
#include <array>
#include <cstdio>
#include <thread>

using namespace extract;

int wmain(int count, wchar_t** arguments) {
    try {
        if (count != 2) return 2;
        for (const auto* name : {L"..", L".", L"a/b", L"a\\b", L"C:stream", L"a:stream", L"trailing.",
             L"trailing ", L"CON.txt", L"con .txt", L"NUL", L"LPT1.bin", L"COM9", L"COM\u00b9.txt", L"a?b", L"a\nb"}) {
            bool rejected = false;
            try { platform::validate_component(name); } catch (const Failure& failure) { rejected = failure.status == Status::unsafe_path; }
            require(rejected, Status::internal_error, L"危险名称未被拒绝。");
        }
        for (const auto* name : {L"中文 文件.txt", L"COM10.bin", L"hello", L"a..b"}) platform::validate_component(name);
        for (const auto* path : {L"../outside.txt", L"C:\\outside.txt", L"\\outside.txt", L"folder/../outside.txt"}) {
            bool rejected = false;
            try { platform::validate_relative(path); } catch (const Failure& failure) { rejected = failure.status == Status::unsafe_path; }
            require(rejected, Status::internal_error, L"危险相对路径未被拒绝。");
        }
        const auto root = platform::absolute_path(arguments[1]) / platform::unique_id();
        {
            Catalog catalog;
            for (const auto letter : {L'a', L'b'}) {
                Entry entry; entry.id.assign(255, letter);
                entry.path = entry.original_path = L"same.txt";
                catalog.files.push_back(std::move(entry));
            }
            plan_paths(catalog);
            require(catalog.files[0].path != catalog.files[1].path, Status::internal_error, L"长标识冲突条目没有分别保留。");
            for (const auto& entry : catalog.files) {
                platform::validate_relative(entry.path);
                require(entry.id.size() == 255 && entry.original_path == L"same.txt", Status::internal_error, L"长标识原始信息丢失。");
            }
        }
        fs::create_directories(root);
        {
            io::Output output(root, L"guard");
            auto file = output.create_file(L"sub/file.txt");
            const std::array<std::byte, 1> data{std::byte{0x41}};
            platform::write_all(file.get(), data);
            file.reset();
            const auto directory = output.full_path(L"sub");
            for (const DWORD access : {static_cast<DWORD>(GENERIC_WRITE), static_cast<DWORD>(DELETE)}) {
                platform::Handle hostile(CreateFileW(platform::extended_path(directory).c_str(), access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                const DWORD error = GetLastError();
                require(!hostile && error == ERROR_SHARING_VIOLATION, Status::internal_error,
                        L"写入阶段目录未被锁定，访问=" + std::to_wstring(access) + L"，句柄=" +
                        std::to_wstring(static_cast<bool>(hostile)) + L"，错误=" + std::to_wstring(error));
            }
        }
        require(fs::is_empty(root), Status::internal_error, L"失败清理残留了文件。");
        {
            io::Output output(root, L"readable-cache");
            auto cache = output.create_file(L"stream.bin", true);
            const auto path = platform::extended_path(output.full_path(L"stream.bin"));
            for (const DWORD access : {static_cast<DWORD>(GENERIC_WRITE), static_cast<DWORD>(DELETE)}) {
                platform::Handle hostile(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                require(!hostile && GetLastError() == ERROR_SHARING_VIOLATION, Status::internal_error, L"可读缓存没有阻止外部写入或替换。");
            }
        }
        require(fs::is_empty(root), Status::internal_error, L"可读缓存清理失败。");
        {
            io::Output output(root, L"transient-lock");
            auto file = output.create_file(L"sub/file.txt");
            std::jthread release([file = std::move(file)]() mutable { Sleep(250); file.reset(); });
            const auto committed = output.commit();
            require(fs::is_regular_file(committed / L"sub/file.txt"), Status::internal_error, L"短暂占用后目录提交失败。");
        }
        std::puts("PASS: unsafe names, traversal, Unicode names, directory locks, cleanup and transient-lock commit.");
        return 0;
    } catch (const Failure& failure) {
        std::fprintf(stderr, "%s\n", platform::utf8(failure.message).c_str());
        return 1;
    } catch (...) { return 1; }
}
