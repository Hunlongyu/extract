#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace extract {
namespace fs = std::filesystem;

enum class Status { unsupported, corrupt, io_error, unsafe_path, limit_exceeded, internal_error };

struct Failure final : std::exception {
    Status status;
    std::wstring message;
    DWORD native_code;
    Failure(Status s, std::wstring m, DWORD code = 0)
        : status(s), message(std::move(m)), native_code(code) {}
    const char* what() const noexcept override { return "Extract failure"; }
};

inline void require(bool condition, Status status, const std::wstring& message) {
    if (!condition) throw Failure(status, message);
}

struct Entry {
    std::wstring id;
    fs::path original_path;
    fs::path path;
    std::uint64_t size = 0;
    std::optional<std::array<DWORD, 4>> msi_hash;
    std::wstring sha256;
    std::wstring source_expression;
    std::wstring conditions;
    std::wstring expected_sha256;
    std::wstring expected_sha1;
    std::wstring expected_sha512;
    std::optional<std::uint32_t> expected_crc32;
    bool source_hash_verified = false;
    bool path_resolved = true;
    bool is_uninstaller = false;
};

struct Catalog {
    std::wstring format = L"MSI";
    std::wstring format_version;
    fs::path input;
    std::wstring cabinet;
    std::vector<Entry> files;
    std::uint64_t total_size = 0;
    bool paths_resolved = true;
    bool content_complete = true;
    bool package_crc_present = false;
    bool package_crc_verified = false;
    struct Nested {
        fs::path input;
        fs::path output;
        std::wstring format;
        std::wstring status;
        std::wstring message;
    };
    std::vector<Nested> nested_packages;
    bool nested_scanned = false;
    bool nested_complete = true;
    std::uint64_t tree_total_bytes = 0;
    std::size_t tree_file_count = 0;
    std::vector<std::wstring> notes;
    bool required_paths_resolved() const;
};

inline constexpr std::size_t max_entries = 10000;
inline constexpr std::uint64_t max_package_bytes = 512ULL * 1024 * 1024;
inline constexpr std::size_t max_cabinet_bytes = 256ULL * 1024 * 1024;
inline constexpr std::uint64_t max_output_bytes = 8ULL * 1024 * 1024 * 1024;

std::wstring catalog_json(const Catalog& catalog, bool extracted);
int exit_code(Status status) noexcept;
} // namespace extract
