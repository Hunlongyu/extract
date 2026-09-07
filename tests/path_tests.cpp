#include "io/output.h"
#include "io/input.h"
#include <winioctl.h>
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
            // 稀疏文件仅占少量磁盘块，验证 x86/x64 都能以 64 位偏移读取。
            const auto path = root / L"large-offset.bin";
            platform::Handle file(CreateFileW(platform::extended_path(path).c_str(), GENERIC_WRITE,
                0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            require(static_cast<bool>(file), Status::internal_error, L"无法创建大文件测试。");
            DWORD unused = 0;
            require(DeviceIoControl(file.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &unused, nullptr) != FALSE,
                Status::internal_error, L"测试磁盘不支持稀疏文件。");
            LARGE_INTEGER offset{}; offset.QuadPart = 5LL * 1024 * 1024 * 1024;
            require(SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN) != FALSE, Status::internal_error, L"大文件定位失败。");
            const std::array<std::byte, 4> expected{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
            platform::write_all(file.get(), expected); file.reset();
            {
                io::Input input(path);
                require(input.size() == static_cast<std::uint64_t>(offset.QuadPart) + 4, Status::internal_error, L"大文件大小截断。");
                std::array<std::byte, 4> actual{};
                input.read(static_cast<std::uint64_t>(offset.QuadPart), actual);
                require(actual == expected, Status::internal_error, L"大文件高位偏移读取错误。");
                bool rejected = false;
                try { input.read(input.size() - 1, actual); } catch (const Failure& e) { rejected = e.status == Status::corrupt; }
                require(rejected, Status::internal_error, L"读取越界未拒绝。");
            }
            fs::remove(path);
            bool rejected = false;
            try { (void)checked_size_sum(max_file_bytes, 1); } catch (const Failure&) { rejected = true; }
            require(rejected, Status::internal_error, L"64 位累计溢出未拒绝。");
            rejected = false;
            try { platform::ensure_disk_space(root, max_file_bytes); }
            catch (const Failure& e) { rejected = e.native_code == ERROR_DISK_FULL; }
            require(rejected, Status::internal_error, L"实际磁盘空间检查未拒绝不可能的申请。");
        }
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
