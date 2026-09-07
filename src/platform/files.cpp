#include "platform/files.h"
#include "platform/log.h"

#include <bcrypt.h>
#include <objbase.h>
#include <winternl.h>
#include <cstring>
#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>

extern "C" NTSYSAPI NTSTATUS NTAPI NtSetInformationFile(
    HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);

namespace extract::platform {
[[noreturn]] void io_failure(const std::wstring& operation, DWORD code) {
    const auto reason = code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL ? L"；磁盘可用空间不足" :
        (code == ERROR_NOT_ENOUGH_MEMORY || code == ERROR_OUTOFMEMORY || code == ERROR_COMMITMENT_LIMIT ? L"；系统内存或地址空间不足" : L"");
    throw Failure(Status::io_error, operation + reason + L"（系统错误 " + std::to_wstring(code) + L"）", code);
}

void ensure_disk_space(const fs::path& directory, std::uint64_t bytes) {
    if (!bytes) return;
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(extended_path(directory).c_str(), &available, nullptr, nullptr)) io_failure(L"无法查询目标磁盘可用空间");
    log::detail(log::Level::info, L"disk.space", [&] {
        return directory.wstring() + L"; required=" + std::to_wstring(bytes) + L"; available=" + std::to_wstring(available.QuadPart);
    });
    if (bytes > available.QuadPart) io_failure(L"磁盘空间不足：" + directory.wstring() +
        L"；预计需要 " + std::to_wstring(bytes) + L" 字节，可用 " + std::to_wstring(available.QuadPart) + L" 字节", ERROR_DISK_FULL);
}

fs::path absolute_path(const fs::path& path) {
    require(!path.empty(), Status::unsafe_path, L"路径不能为空。");
    std::wstring text = path.wstring();
    require(text.find(L'\0') == text.npos, Status::unsafe_path, L"路径包含空字符。");
    if (text.starts_with(L"\\\\?\\")) text.erase(0, 4);
    require(!text.starts_with(L"\\\\"), Status::unsupported, L"首版只支持本地磁盘路径。");
    const DWORD length = GetFullPathNameW(text.c_str(), 0, nullptr, nullptr);
    if (length == 0) io_failure(L"无法解析路径");
    require(length < 32000, Status::limit_exceeded, L"路径过长。");
    std::vector<wchar_t> buffer(length);
    const DWORD written = GetFullPathNameW(text.c_str(), length, buffer.data(), nullptr);
    if (written == 0 || written >= length) io_failure(L"无法解析完整路径");
    fs::path result(std::wstring(buffer.data(), written));
    while (result.has_relative_path() && result.filename().empty()) result = result.parent_path();
    require(result.root_name().wstring().size() == 2 && result.root_name().wstring()[1] == L':',
            Status::unsupported, L"只支持本地驱动器路径。");
    for (const auto& part : result.relative_path()) validate_component(part.wstring());
    return result;
}

std::wstring extended_path(const fs::path& path) {
    auto native = path;
    native.make_preferred();
    return L"\\\\?\\" + native.wstring();
}

DWORD rename_in_place(HANDLE file, std::wstring_view name, bool replace) {
    validate_component(name);
    // Native API 的单个名称表示原父目录内重命名，不重新打开路径中的目录。
    // Win32 的完整目标路径重命名会额外打开父目录，与写入保护锁冲突。
    struct RenameInfo { BOOLEAN replace; HANDLE root; ULONG bytes; WCHAR name[1]; };
    const auto bytes = name.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(RenameInfo) + bytes);
    auto* info = reinterpret_cast<RenameInfo*>(buffer.data());
    info->replace = replace ? TRUE : FALSE;
    info->bytes = static_cast<ULONG>(bytes);
    std::memcpy(info->name, name.data(), bytes);
    IO_STATUS_BLOCK status{};
    const auto result = NtSetInformationFile(file, &status, info, static_cast<ULONG>(buffer.size()),
                                           static_cast<FILE_INFORMATION_CLASS>(10));
    return result < 0 ? RtlNtStatusToDosError(result) : ERROR_SUCCESS;
}

bool equal_name(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

void validate_component(std::wstring_view name) {
    require(!name.empty() && name.size() <= 255 && name != L"." && name != L"..",
            Status::unsafe_path, L"文件或目录名称无效。");
    require(name.back() != L'.' && name.back() != L' ', Status::unsafe_path, L"名称不能以点或空格结尾。");
    for (wchar_t c : name)
        require(c >= 0x20 && std::wstring_view(L"<>:\"/\\|?*").find(c) == std::wstring_view::npos,
                Status::unsafe_path, L"名称包含路径分隔符或非法字符：" + std::wstring(name));
    auto base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ') base.remove_suffix(1);
    for (const auto reserved : {L"CON", L"PRN", L"AUX", L"NUL", L"CLOCK$", L"CONIN$", L"CONOUT$"})
        require(!equal_name(base, reserved), Status::unsafe_path, L"拒绝 Windows 设备名称。");
    if (base.size() == 4 && (equal_name(base.substr(0, 3), L"COM") || equal_name(base.substr(0, 3), L"LPT"))) {
        const wchar_t digit = base[3];
        require(!((digit >= L'1' && digit <= L'9') || digit == L'\u00b9' || digit == L'\u00b2' || digit == L'\u00b3'),
                Status::unsafe_path, L"拒绝 Windows 设备名称。");
    }
    (void)utf8(name);
}

void validate_relative(const fs::path& path) {
    require(!path.empty() && !path.has_root_path(), Status::unsafe_path, L"输出必须是相对路径。");
    for (const auto& part : path) validate_component(part.wstring());
    require(path.wstring().size() < 16000, Status::limit_exceeded, L"输出相对路径过长。");
}

std::wstring unique_id() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw Failure(Status::internal_error, L"无法生成任务标识。");
    wchar_t text[40]{};
    StringFromGUID2(guid, text, 40);
    return std::wstring(text + 1, 36);
}

std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    require(text.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)()),
            Status::limit_exceeded, L"文本过长。");
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    require(count > 0, Status::corrupt, L"文本包含无效 Unicode 字符。");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                           result.data(), count, nullptr, nullptr) != count) io_failure(L"文本编码失败");
    return result;
}

void write_all(HANDLE file, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const auto chunk = static_cast<DWORD>((std::min)(bytes.size(), std::size_t{1 << 20}));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data(), chunk, &written, nullptr) || written == 0) io_failure(L"写入文件失败");
        bytes = bytes.subspan(written);
    }
}

static std::wstring file_hash(const fs::path& path, LPCWSTR algorithm_name, ULONG digest_size) {
    log::Scope step(L"file.hash", nullptr, &path);
    log::write(log::Level::info, L"hash.algorithm", algorithm_name);
    Handle file(CreateFileW(extended_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file) io_failure(L"无法读取待校验文件");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, algorithm_name, nullptr, 0) < 0)
        throw Failure(Status::internal_error, L"无法初始化文件摘要算法。");
    struct AlgorithmGuard { BCRYPT_ALG_HANDLE value; ~AlgorithmGuard() { BCryptCloseAlgorithmProvider(value, 0); } } guard{algorithm};
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw Failure(Status::internal_error, L"无法创建文件摘要状态。");
    struct HashGuard { BCRYPT_HASH_HANDLE value; ~HashGuard() { BCryptDestroyHash(value); } } hash_guard{hash};
    std::array<UCHAR, 65536> bytes{};
    DWORD read = 0;
    for (;;) {
        if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) io_failure(L"校验读取失败");
        if (read == 0) break;
        if (BCryptHashData(hash, bytes.data(), read, 0) < 0) throw Failure(Status::internal_error, L"文件摘要计算失败。");
    }
    std::vector<UCHAR> digest(digest_size);
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw Failure(Status::internal_error, L"文件摘要计算失败。");
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    for (auto c : digest) { result += hex[c >> 4]; result += hex[c & 15]; }
    return result;
}

std::wstring sha256(const fs::path& path) { return file_hash(path, BCRYPT_SHA256_ALGORITHM, 32); }
std::wstring sha1(const fs::path& path) { return file_hash(path, BCRYPT_SHA1_ALGORITHM, 20); }
std::wstring sha512(const fs::path& path) { return file_hash(path, BCRYPT_SHA512_ALGORITHM, 64); }
std::wstring hash_bytes(std::span<const std::byte> bytes, std::size_t size) {
    require(size == 20 || size == 32 || size == 64, Status::unsupported, L"不支持此哈希算法。");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    const auto name = size == 20 ? BCRYPT_SHA1_ALGORITHM : (size == 32 ? BCRYPT_SHA256_ALGORITHM : BCRYPT_SHA512_ALGORITHM);
    require(BCryptOpenAlgorithmProvider(&algorithm, name, nullptr, 0) >= 0, Status::internal_error, L"无法初始化哈希。");
    struct Guard { BCRYPT_ALG_HANDLE value; ~Guard() { BCryptCloseAlgorithmProvider(value, 0); } } guard{algorithm};
    BCRYPT_HASH_HANDLE hash = nullptr;
    require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0, Status::internal_error, L"无法创建哈希。");
    struct HashGuard { BCRYPT_HASH_HANDLE value; ~HashGuard() { BCryptDestroyHash(value); } } hash_guard{hash};
    while (!bytes.empty()) {
        const auto count = (std::min)(bytes.size(), std::size_t{65536});
        require(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())), static_cast<ULONG>(count), 0) >= 0,
                Status::internal_error, L"哈希计算失败。");
        bytes = bytes.subspan(count);
    }
    std::array<unsigned char, 64> digest{};
    require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(size), 0) >= 0, Status::internal_error, L"哈希计算失败。");
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    for (std::size_t i = 0; i < size; ++i) { result += hex[digest[i] >> 4]; result += hex[digest[i] & 15]; }
    return result;
}

Handle lock_directory(const fs::path& path, bool rename) {
    // FILE_READ_ATTRIBUTES 单独使用不参与共享访问检查；目录读取权限使共享写入/删除限制生效。
    Handle handle(CreateFileW(extended_path(path).c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | (rename ? DELETE : 0),
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle) io_failure(L"无法锁定目录：" + path.wstring());
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &info, sizeof(info))) io_failure(L"无法检查目录");
    require((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
            Status::unsafe_path, L"拒绝通过重解析点或非目录输出文件。");
    return handle;
}

std::vector<Handle> lock_ancestors(const fs::path& path) {
    std::vector<Handle> locks;
    fs::path current = path.root_path();
    locks.push_back(lock_directory(current));
    for (const auto& part : path.relative_path()) {
        current /= part;
        locks.push_back(lock_directory(current));
    }
    return locks;
}
} // namespace extract::platform
