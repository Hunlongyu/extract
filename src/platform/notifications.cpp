#include "platform/notifications.h"
#include "platform/jobs.h"
#include "platform/console.h"

#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>

namespace extract::platform {
namespace {
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace ABI::Windows::UI::Notifications;
using namespace ABI::Windows::Data::Xml::Dom;
constexpr wchar_t app_id[] = L"Hunlongyu.Extract";
constexpr wchar_t class_id[] = L"{81A1EB43-E598-4550-9F17-561103A29D7F}";
constexpr GUID activator_id = {0x81a1eb43, 0xe598, 0x4550, {0x9f, 0x17, 0x56, 0x11, 0x03, 0xa2, 0x9d, 0x7f}};
constexpr wchar_t protocol_key[] = L"Software\\Classes\\hunlongyu-extract";

void check(HRESULT result, const wchar_t* operation) {
    if (FAILED(result)) throw Failure(Status::io_error,
        std::wstring(operation) + L"（HRESULT " + std::to_wstring(static_cast<unsigned long>(result)) + L"）", static_cast<DWORD>(result));
}
fs::path shortcut_path() {
    PWSTR value = nullptr;
    check(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &value), L"定位通知快捷方式失败");
    struct Guard { PWSTR p; ~Guard() { CoTaskMemFree(p); } } guard{value};
    return fs::path(value) / L"Extract (Hunlongyu).lnk";
}
bool owns_shortcut(const fs::path& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    ComPtr<IShellLinkW> link;
    check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)), L"创建快捷方式接口失败");
    ComPtr<IPersistFile> file;
    check(link.As(&file), L"读取快捷方式接口失败");
    check(file->Load(path.c_str(), STGM_READ), L"读取现有快捷方式失败");
    ComPtr<IPropertyStore> properties;
    check(link.As(&properties), L"读取快捷方式属性失败");
    PROPVARIANT value{};
    check(properties->GetValue(PKEY_AppUserModel_ID, &value), L"读取快捷方式标识失败");
    const bool own = value.vt == VT_LPWSTR && value.pwszVal && std::wstring_view(value.pwszVal) == app_id;
    PropVariantClear(&value);
    return own;
}
void registry_text(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    const LSTATUS opened = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                                          KEY_SET_VALUE, nullptr, &key, nullptr);
    if (opened != ERROR_SUCCESS) io_failure(L"通知注册失败", opened);
    struct Guard { HKEY value; ~Guard() { RegCloseKey(value); } } guard{key};
    const auto status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) io_failure(L"保存通知注册失败", status);
}
std::wstring xml_escape(std::wstring_view text) {
    std::wstring escaped;
    for (wchar_t c : text) {
        switch (c) {
        case L'&': escaped += L"&amp;"; break;
        case L'<': escaped += L"&lt;"; break;
        case L'>': escaped += L"&gt;"; break;
        case L'"': escaped += L"&quot;"; break;
        case L'\'': escaped += L"&apos;"; break;
        default: if (c >= 0x20 || c == L'\n' || c == L'\r' || c == L'\t') escaped += c; break;
        }
    }
    return escaped;
}

void delete_tree(const std::wstring& path) {
    HKEY key = nullptr;
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ | KEY_WRITE, &key);
    if (opened == ERROR_FILE_NOT_FOUND) return;
    if (opened != ERROR_SUCCESS) io_failure(L"打开通知注册项失败", opened);
    const auto cleared = RegDeleteTreeW(key, nullptr);
    RegCloseKey(key);
    if (cleared != ERROR_SUCCESS) io_failure(L"清理通知注册内容失败", cleared);
    const auto removed = RegDeleteKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, 0);
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) io_failure(L"清理通知注册项失败", removed);
}
bool owns_protocol() {
    wchar_t owner[128]{};
    DWORD size = sizeof(owner);
    const auto result = RegGetValueW(HKEY_CURRENT_USER, protocol_key, L"ExtractOwner", RRF_RT_REG_SZ, nullptr, owner, &size);
    if (result == ERROR_FILE_NOT_FOUND) {
        HKEY key = nullptr;
        const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, protocol_key, 0, KEY_READ, &key);
        if (key) RegCloseKey(key);
        return opened == ERROR_FILE_NOT_FOUND;
    }
    return result == ERROR_SUCCESS && std::wstring_view(owner) == app_id;
}
}

void register_notifications() {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) io_failure(L"获取程序位置失败");
    executable.resize(length);
    const auto shortcut = shortcut_path();
    require(owns_shortcut(shortcut), Status::io_error, L"通知快捷方式名称被其它程序占用。");
    require(owns_protocol(), Status::io_error, L"通知协议名称被其它程序占用。");
    auto guards = lock_ancestors(absolute_path(shortcut.parent_path()));
    ComPtr<IShellLinkW> link;
    check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)), L"创建通知快捷方式失败");
    check(link->SetPath(executable.c_str()), L"设置通知程序位置失败");
    check(link->SetWorkingDirectory(fs::path(executable).parent_path().c_str()), L"设置工作目录失败");
    check(link->SetDescription(L"Extract 安装包解包工具"), L"设置快捷方式说明失败");
    ComPtr<IPropertyStore> properties;
    check(link.As(&properties), L"创建通知属性失败");
    PROPVARIANT value{};
    check(InitPropVariantFromString(app_id, &value), L"创建通知标识失败");
    const auto id_result = properties->SetValue(PKEY_AppUserModel_ID, value);
    PropVariantClear(&value);
    check(id_result, L"设置通知标识失败");
    check(InitPropVariantFromCLSID(activator_id, &value), L"创建激活标识失败");
    const auto clsid_result = properties->SetValue(PKEY_AppUserModel_ToastActivatorCLSID, value);
    PropVariantClear(&value);
    check(clsid_result, L"设置通知激活器失败");
    check(properties->Commit(), L"保存通知属性失败");
    ComPtr<IPersistFile> file;
    check(link.As(&file), L"创建快捷方式保存接口失败");
    check(file->Save(shortcut.c_str(), TRUE), L"保存通知快捷方式失败");
    // 协议激活使用快捷方式中的占位 CLSID，无需常驻 COM 服务器。
    registry_text(protocol_key, L"ExtractOwner", app_id);
    registry_text(protocol_key, nullptr, L"URL:Extract result");
    registry_text(protocol_key, L"URL Protocol", L"");
    registry_text(std::wstring(protocol_key) + L"\\shell\\open\\command", nullptr,
                  L"\"" + executable + L"\" --open-notification \"%1\"");
    const std::wstring application = std::wstring(L"Software\\Classes\\AppUserModelId\\") + app_id;
    registry_text(application, L"DisplayName", L"Extract");
    registry_text(application, L"CustomActivator", class_id);
    check(SetCurrentProcessExplicitAppUserModelID(app_id), L"设置进程通知标识失败");
}

void unregister_notifications() {
    ComPtr<IToastNotificationManagerStatics2> manager;
    if (SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(), IID_PPV_ARGS(&manager)))) {
        ComPtr<IToastNotificationHistory> history;
        if (SUCCEEDED(manager->get_History(&history))) (void)history->ClearWithId(HStringReference(app_id).Get());
    }
    const auto shortcut = shortcut_path();
    require(owns_shortcut(shortcut), Status::io_error, L"快捷方式不属于本程序，未删除。");
    if (!DeleteFileW(shortcut.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) io_failure(L"删除通知快捷方式失败");
    require(owns_protocol(), Status::io_error, L"协议注册不属于本程序，未删除。");
    delete_tree(protocol_key);
    delete_tree(std::wstring(L"Software\\Classes\\AppUserModelId\\") + app_id);
}

HRESULT show_notification(std::wstring_view title, std::wstring_view body, std::wstring_view job_id) noexcept {
    try {
        require(job_id.empty() || valid_job_id(job_id), Status::internal_error, L"通知任务标识无效。");
        register_notifications();
        ComPtr<IToastNotificationManagerStatics> manager;
        check(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(), IID_PPV_ARGS(&manager)), L"创建系统通知管理器失败");
        ComPtr<IToastNotifier> notifier;
        check(manager->CreateToastNotifierWithId(HStringReference(app_id).Get(), &notifier), L"创建系统通知发送器失败");
        NotificationSetting setting{};
        // 新 AUMID 在首次发送前可能尚无通知设置记录，仍尝试提交。
        if (SUCCEEDED(notifier->get_Setting(&setting)) && setting != NotificationSetting_Enabled) return S_FALSE;
        const auto uri = job_id.empty() ? std::wstring(L"hunlongyu-extract://help/") : L"hunlongyu-extract://job/" + std::wstring(job_id);
        const auto xml = L"<toast activationType=\"protocol\" launch=\"" + xml_escape(uri) + L"\"><visual><binding template=\"ToastGeneric\"><text>"
            + xml_escape(title) + L"</text><text>" + xml_escape(body.substr(0, 500))
            + L"</text></binding></visual><audio silent=\"true\"/></toast>";
        ComPtr<IInspectable> inspectable;
        check(RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), &inspectable), L"创建通知 XML 失败");
        ComPtr<IXmlDocument> document;
        ComPtr<IXmlDocumentIO> document_io;
        check(inspectable.As(&document), L"获取通知 XML 文档失败");
        check(inspectable.As(&document_io), L"获取通知 XML 读写接口失败");
        check(document_io->LoadXml(HStringReference(xml.c_str()).Get()), L"解析通知 XML 失败");
        ComPtr<IToastNotificationFactory> factory;
        check(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(), IID_PPV_ARGS(&factory)), L"创建通知工厂失败");
        ComPtr<IToastNotification> notification;
        check(factory->CreateToastNotification(document.Get(), &notification), L"创建通知失败");
        return notifier->Show(notification.Get());
    } catch (const Failure& failure) {
        write_diagnostic(failure.message + L"\r\n", true);
        return failure.native_code != 0 ? static_cast<HRESULT>(failure.native_code) : E_FAIL;
    } catch (...) { return E_FAIL; }
}

void activate_notification(std::wstring_view uri) {
    if (uri == L"hunlongyu-extract://help" || uri == L"hunlongyu-extract://help/") return;
    constexpr std::wstring_view prefix = L"hunlongyu-extract://job/";
    require(uri.starts_with(prefix) && valid_job_id(uri.substr(prefix.size())),
            Status::unsafe_path, L"通知链接无效。");
    open_job(uri.substr(prefix.size()));
}
} // namespace extract::platform
