#include "core/types.h"

namespace extract {
namespace {
std::wstring json_string(std::wstring_view text) {
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result = L"\"";
    for (wchar_t c : text) {
        if (c == L'"' || c == L'\\') { result += L'\\'; result += c; }
        else if (c < 0x20) {
            result += L"\\u00";
            result += hex[(c >> 4) & 15]; result += hex[c & 15];
        } else result += c;
    }
    return result + L'"';
}
}

bool Catalog::required_paths_resolved() const {
    if (paths_resolved) return true;
    bool has_unresolved = false;
    for (const auto& file : files) if (!file.path_resolved) {
        has_unresolved = true;
        if (!file.is_uninstaller) return false;
    }
    // 未提供逐文件原因的处理器仍按原有的未完成状态报告。
    return has_unresolved;
}

std::wstring catalog_json(const Catalog& catalog, bool extracted) {
    std::wstring text = L"{\n  \"format\": " + json_string(catalog.format)
        + L",\n  \"formatVersion\": " + json_string(catalog.format_version) + L",\n  \"status\": "
        + json_string(extracted ? (catalog.required_paths_resolved() && catalog.content_complete && catalog.nested_complete ? L"complete" : L"partial") : L"listed")
        + L",\n  \"input\": " + json_string(catalog.input.wstring())
        + L",\n  \"layout\": \"static logical directories\",\n  \"cabinet\": "
        + json_string(catalog.cabinet) + L",\n  \"totalBytes\": " + std::to_wstring(catalog.total_size)
        + L",\n  \"pathsResolved\": " + (catalog.paths_resolved ? L"true" : L"false")
        + L",\n  \"requiredPathsResolved\": " + (catalog.required_paths_resolved() ? L"true" : L"false")
        + L",\n  \"contentComplete\": " + (catalog.content_complete ? L"true" : L"false")
        + L",\n  \"packageCrcPresent\": " + (catalog.package_crc_present ? L"true" : L"false")
        + L",\n  \"packageCrcVerified\": " + (catalog.package_crc_verified ? L"true" : L"false")
        + L",\n  \"nestedScanned\": " + (catalog.nested_scanned ? L"true" : L"false")
        + L",\n  \"nestedComplete\": " + (catalog.nested_complete ? L"true" : L"false")
        + L",\n  \"treeTotalBytes\": " + std::to_wstring(catalog.tree_total_bytes)
        + L",\n  \"treeFileCount\": " + std::to_wstring(catalog.tree_file_count)
        + L",\n  \"notes\": [";
    for (std::size_t i = 0; i < catalog.notes.size(); ++i) {
        if (i) text += L", ";
        text += json_string(catalog.notes[i]);
    }
    text += L"],\n  \"nestedPackages\": [";
    for (std::size_t i = 0; i < catalog.nested_packages.size(); ++i) {
        const auto& nested = catalog.nested_packages[i];
        if (i) text += L", ";
        text += L"{\"input\": " + json_string(nested.input.generic_wstring())
            + L", \"output\": " + json_string(nested.output.generic_wstring())
            + L", \"format\": " + json_string(nested.format)
            + L", \"status\": " + json_string(nested.status)
            + L", \"message\": " + json_string(nested.message) + L"}";
    }
    text += L"],\n  \"files\": [\n";
    bool first = true;
    for (const auto& file : catalog.files) {
        if (!first) text += L",\n";
        first = false;
        text += L"    {\"id\": " + json_string(file.id)
            + L", \"originalPath\": " + json_string(file.original_path.generic_wstring())
            + L", \"path\": " + json_string(file.path.generic_wstring())
            + L", \"pathResolved\": " + (file.path_resolved ? L"true" : L"false")
            + L", \"role\": " + json_string(file.is_uninstaller ? L"uninstaller" : L"payload")
            + L", \"size\": " + std::to_wstring(file.size)
            + L", \"sha256\": " + json_string(file.sha256)
            + L", \"sourceExpression\": " + json_string(file.source_expression)
            + L", \"conditions\": " + json_string(file.conditions)
            + L", \"sourceHashAlgorithm\": " + json_string(!file.expected_sha512.empty() ? L"SHA-512" : (!file.expected_sha1.empty() ? L"SHA-1" : (!file.expected_sha256.empty() ? L"SHA-256" : (file.expected_crc32 ? L"CRC-32" : L""))))
            + L", \"sourceHashVerified\": " + (extracted && file.source_hash_verified ? L"true" : L"false")
            + L", \"blockHashAlgorithm\": " + json_string(file.block_hash_algorithm)
            + L", \"blockHashVerified\": " + (extracted && file.block_hash_verified ? L"true" : L"false")
            + L", \"msiHashVerified\": " + (extracted && file.msi_hash ? L"true" : L"false") + L"}";
    }
    return text + L"\n  ]\n}\n";
}

int exit_code(Status status) noexcept {
    switch (status) {
    case Status::unsupported: return ERROR_NOT_SUPPORTED;
    case Status::corrupt: return ERROR_INVALID_DATA;
    case Status::io_error: return ERROR_READ_FAULT;
    case Status::unsafe_path: return ERROR_ACCESS_DENIED;
    case Status::limit_exceeded: return ERROR_FILE_TOO_LARGE;
    case Status::cancelled: return ERROR_CANCELLED;
    case Status::timeout: return ERROR_TIMEOUT;
    case Status::worker_crashed: return ERROR_PROCESS_ABORTED;
    default: return ERROR_UNHANDLED_EXCEPTION;
    }
}
} // namespace extract
