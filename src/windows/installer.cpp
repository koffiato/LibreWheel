// SPDX-License-Identifier: GPL-3.0-or-later
#include "hid.hpp"
#include "installer_resource.h"
#include "output_lease.hpp"
#include "payload_archive.hpp"
#include "registration.hpp"
#include "version.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t app_title[] = L"LibreWheel Setup";
constexpr wchar_t tray_window_class[] = L"LibreWheel.Tray.Window";
constexpr wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t run_value[] = L"LibreWheelTray";
constexpr wchar_t uninstall_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\LibreWheel";

std::wstring version_string() { return std::wstring(lw::version::wide); }

constexpr int btn_install = 1001;
constexpr int btn_remove_oem = 1002;
constexpr int btn_keep_oem = 1003;
constexpr int btn_keep_confirm = 1004;
constexpr int btn_go_back = 1005;
constexpr int btn_uninstall = 1006;

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring(utf8.begin(), utf8.end());
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

bool equals_icase(std::wstring_view a, std::wstring_view b) noexcept {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::filesystem::path current_executable_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!size || size >= buffer.size()) {
        throw std::runtime_error("Unable to resolve installer executable path");
    }
    buffer.resize(size);
    return std::filesystem::path(buffer);
}

std::filesystem::path local_appdata_dir() {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!length) {
        throw std::runtime_error("Unable to resolve LOCALAPPDATA");
    }
    std::wstring dir(length, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", dir.data(), length);
    if (!copied || copied >= length) {
        throw std::runtime_error("Unable to read LOCALAPPDATA");
    }
    dir.resize(copied);
    return std::filesystem::path(dir);
}

std::filesystem::path default_install_dir() {
    return local_appdata_dir() / L"Programs" / L"LibreWheel";
}

std::filesystem::path default_config_dir() {
    return local_appdata_dir() / L"LibreWheel";
}

std::optional<std::filesystem::path> start_menu_shortcut_path() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &raw)) || !raw) {
        return std::nullopt;
    }
    struct FreeTaskMem {
        PWSTR ptr;
        ~FreeTaskMem() { CoTaskMemFree(ptr); }
    } owned{raw};
    return std::filesystem::path(raw) / L"LibreWheel.lnk";
}

bool apps_use_dark_mode() noexcept {
    DWORD value = 1;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS) {
        return value == 0;
    }
    return false;
}

void enable_dark_title_bar(HWND hwnd) noexcept {
    if (!hwnd || !apps_use_dark_mode()) {
        return;
    }
    const BOOL enabled = TRUE;
    constexpr DWORD dwmwa_immersive_dark_mode = 20;
    DwmSetWindowAttribute(hwnd, dwmwa_immersive_dark_mode, &enabled, sizeof(enabled));
}

void init_dark_mode_theme() noexcept {
    if (const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using SetPreferredAppModeFn = int(WINAPI*)(int);
#pragma warning(push)
#pragma warning(disable: 4191)
        const auto set_preferred =
            reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
#pragma warning(pop)
        if (set_preferred) {
            set_preferred(1); // AllowDark
        }
    }
}

struct DialogCallbackData {
    int shield_button_id = 0;
};

HRESULT CALLBACK task_dialog_callback(HWND hwnd, UINT notification, WPARAM, LPARAM, LONG_PTR ref_data) noexcept {
    if (notification == TDN_DIALOG_CONSTRUCTED) {
        enable_dark_title_bar(hwnd);
        if (const auto* data = reinterpret_cast<const DialogCallbackData*>(ref_data);
            data && data->shield_button_id != 0) {
            SendMessageW(hwnd, TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE,
                         static_cast<WPARAM>(data->shield_button_id), TRUE);
        }
    }
    return S_OK;
}

void show_message(const wchar_t* title, const wchar_t* heading, const std::wstring& body,
                  PCWSTR icon, bool silent) noexcept {
    if (silent) {
        return;
    }
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hInstance = GetModuleHandleW(nullptr);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    config.pszWindowTitle = title;
    config.pszMainIcon = icon;
    config.pszMainInstruction = heading;
    config.pszContent = body.c_str();
    config.pfCallback = task_dialog_callback;
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

void stop_running_tray_and_driver() noexcept {
    if (const HWND window = FindWindowW(tray_window_class, nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        const auto process = pid ? lw::win::handle(OpenProcess(SYNCHRONIZE, FALSE, pid)) : lw::win::Handle{};
        PostMessageW(window, WM_CANCELMODE, 0, 0);
        PostMessageW(window, WM_CLOSE, 0, 0);
        if (process) {
            WaitForSingleObject(process.get(), 5000);
        }
    }
    if (const auto stop_event = lw::win::handle(CreateEventW(nullptr, TRUE, FALSE, lw::win::user_stop_event_name))) {
        SetEvent(stop_event.get());
        if (const auto mutex = lw::win::handle(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                                          lw::win::output_mutex_name))) {
            const auto wait = WaitForSingleObject(mutex.get(), 500);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
                ReleaseMutex(mutex.get());
            }
        }
        Sleep(75);
        ResetEvent(stop_event.get());
    }
}

std::vector<lw::win::PayloadEntry> load_embedded_payload() {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_PAYLOAD), RT_RCDATA);
    if (!resource) {
        throw std::runtime_error("Installer payload resource is missing");
    }
    const DWORD size = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const void* raw = loaded ? LockResource(loaded) : nullptr;
    if (!raw || size == 0) {
        throw std::runtime_error("Installer payload resource could not be loaded");
    }
    const std::span<const std::uint8_t> bytes(static_cast<const std::uint8_t*>(raw), size);
    return lw::win::unpack_payload(bytes);
}

void write_file_atomic(const std::filesystem::path& destination, std::span<const std::uint8_t> data, bool silent) {
    if (destination.has_parent_path()) {
        std::filesystem::create_directories(destination.parent_path());
    }
    const auto temp = destination.wstring() + L".new";
    while (true) {
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("Unable to stage file: " + destination.string());
            }
            if (!data.empty()) {
                out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            }
            out.close();
            if (!out) {
                DeleteFileW(temp.c_str());
                throw std::runtime_error("Unable to write staged file: " + destination.string());
            }
        }
        if (MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return;
        }
        const DWORD error = GetLastError();
        DeleteFileW(temp.c_str());
        if (silent || (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED)) {
            throw std::runtime_error("Unable to replace file (close running games first): " + destination.string());
        }
        const std::wstring prompt =
            L"Could not update:\n" + destination.wstring() +
            L"\n\nClose any running games or programs using LibreWheel and click Retry.";
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hInstance = GetModuleHandleW(nullptr);
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        config.dwCommonButtons = TDCBF_RETRY_BUTTON | TDCBF_CANCEL_BUTTON;
        config.pszWindowTitle = app_title;
        config.pszMainIcon = TD_WARNING_ICON;
        config.pszMainInstruction = L"File in use";
        config.pszContent = prompt.c_str();
        config.pfCallback = task_dialog_callback;
        int button = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&config, &button, nullptr, nullptr)) || button != IDRETRY) {
            throw std::runtime_error("Installation cancelled because a LibreWheel file is in use");
        }
    }
}

void create_shortcut(const std::filesystem::path& link_path, const std::filesystem::path& target,
                     const std::filesystem::path& working_dir) {
    IShellLinkW* raw_link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&raw_link))) || !raw_link) {
        return;
    }
    struct ReleaseLink {
        IShellLinkW* ptr;
        ~ReleaseLink() { ptr->Release(); }
    } link{raw_link};
    link.ptr->SetPath(target.c_str());
    link.ptr->SetWorkingDirectory(working_dir.c_str());
    link.ptr->SetDescription(L"LibreWheel Logitech G29 Force-Feedback Driver");
    link.ptr->SetIconLocation(target.c_str(), 0);

    IPersistFile* raw_file = nullptr;
    if (SUCCEEDED(link.ptr->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&raw_file))) && raw_file) {
        raw_file->Save(link_path.c_str(), TRUE);
        raw_file->Release();
    }
}

void write_uninstall_entry(const std::filesystem::path& install_dir, std::size_t installed_bytes) {
    HKEY raw = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, uninstall_key, 0, nullptr, 0,
                        KEY_WRITE | KEY_WOW64_64KEY, nullptr, &raw, nullptr) != ERROR_SUCCESS) {
        return;
    }
    struct CloseReg {
        HKEY h;
        ~CloseReg() { RegCloseKey(h); }
    } key{raw};
    const auto set_sz = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key.h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    const auto set_dw = [&](const wchar_t* name, DWORD value) {
        RegSetValueExW(key.h, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    };

    const auto uninstaller = install_dir / L"Uninstall.exe";
    const auto tray = install_dir / L"LibreWheelTray.exe";
    set_sz(L"DisplayName", L"LibreWheel");
    set_sz(L"DisplayVersion", version_string());
    set_sz(L"Publisher", L"LibreWheel Contributors");
    set_sz(L"URLInfoAbout", L"https://github.com/koffiato/LibreWheel");
    set_sz(L"DisplayIcon", tray.wstring());
    set_sz(L"InstallLocation", install_dir.wstring());
    set_sz(L"UninstallString", L"\"" + uninstaller.wstring() + L"\" --uninstall");
    set_sz(L"QuietUninstallString", L"\"" + uninstaller.wstring() + L"\" --uninstall --silent");
    set_dw(L"EstimatedSize", static_cast<DWORD>(std::max<std::size_t>(1, (installed_bytes + 1023) / 1024)));
    set_dw(L"NoModify", 1);
    set_dw(L"NoRepair", 1);
}

void run_elevated_hklm_removal(const std::filesystem::path& backup_dir) {
    const auto self = current_executable_path();
    const std::wstring params = L"--remove-hklm \"" + backup_dir.wstring() + L"\"";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = self.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        throw std::runtime_error(
            "Administrator permission is required to remove the machine-wide (HKLM) Logitech/OEM G29 registration.");
    }
    const auto process = lw::win::handle(info.hProcess);
    WaitForSingleObject(process.get(), INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.get(), &exit_code);
    if (exit_code != 0 || lw::win::has_oem_conflict(true)) {
        throw std::runtime_error(
            "Could not remove the machine-wide (HKLM) Logitech/OEM G29 registration.");
    }
}

std::wstring format_conflicts(const std::vector<lw::win::ConflictInfo>& conflicts) {
    std::wstring text;
    for (const auto& conflict : conflicts) {
        if (!text.empty()) {
            text += L"\n";
        }
        text += conflict.machine_hive ? L"  \u2022 HKLM (machine-wide)" : L"  \u2022 HKCU (current user)";
        if (!conflict.clsid.empty()) {
            text += L": " + conflict.clsid;
        }
        if (!conflict.server_path.empty()) {
            text += L" (" + conflict.server_path + L")";
        }
    }
    return text;
}

enum class ConflictChoice { cancel, remove_and_register, keep_files_only };

ConflictChoice prompt_conflict_choice(const std::vector<lw::win::ConflictInfo>& conflicts) {
    const bool needs_uac = std::ranges::any_of(conflicts, [](const auto& c) { return c.machine_hive; }) &&
                           !lw::win::is_process_elevated();

    const std::wstring remove_label =
        std::wstring(L"Remove existing Logitech/OEM registration and install LibreWheel (Recommended)\n") +
        (needs_uac
            ? L"Saves a .reg backup to %LOCALAPPDATA%\\LibreWheel\\backups\\ first (prompts for Administrator permission)."
            : L"Saves a .reg backup to %LOCALAPPDATA%\\LibreWheel\\backups\\ before removal.");
    const std::wstring keep_label =
        L"Keep existing registration (install files only)\n"
        L"Warning: LibreWheel force feedback will NOT work until the existing registration is removed.";

    const std::wstring content =
        L"An existing Logitech/OEM force-feedback registration for the G29 was detected:\n" +
        format_conflicts(conflicts) +
        L"\n\nLibreWheel's DirectInput force-feedback driver will NOT work while this registration remains in place.";

    while (true) {
        const TASKDIALOG_BUTTON buttons[]{
            {btn_remove_oem, remove_label.c_str()},
            {btn_keep_oem, keep_label.c_str()},
        };
        DialogCallbackData cb_data{needs_uac ? btn_remove_oem : 0};
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hInstance = GetModuleHandleW(nullptr);
        config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        config.pszWindowTitle = app_title;
        config.pszMainIcon = TD_WARNING_ICON;
        config.pszMainInstruction = L"Conflicting G29 force-feedback driver detected";
        config.pszContent = content.c_str();
        config.cButtons = static_cast<UINT>(std::size(buttons));
        config.pButtons = buttons;
        config.nDefaultButton = btn_remove_oem;
        config.pfCallback = task_dialog_callback;
        config.lpCallbackData = reinterpret_cast<LONG_PTR>(&cb_data);

        int choice = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr))) {
            return ConflictChoice::cancel;
        }
        if (choice == btn_remove_oem) {
            return ConflictChoice::remove_and_register;
        }
        if (choice != btn_keep_oem) {
            return ConflictChoice::cancel;
        }

        // Explicit secondary warning before skipping DirectInput registration.
        const TASKDIALOG_BUTTON confirm_buttons[]{
            {btn_keep_confirm, L"Install files only (no force feedback)"},
            {btn_go_back, L"Go back"},
        };
        TASKDIALOGCONFIG confirm{};
        confirm.cbSize = sizeof(confirm);
        confirm.hInstance = GetModuleHandleW(nullptr);
        confirm.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        confirm.pszWindowTitle = app_title;
        confirm.pszMainIcon = TD_WARNING_ICON;
        confirm.pszMainInstruction = L"LibreWheel will not work without removing the Logitech registration";
        confirm.pszContent =
            L"Without removing the existing Logitech/OEM G29 force-feedback registration, LibreWheel's DirectInput "
            L"driver will not load in games.\n\n"
            L"LibreWheel files will still be installed, but DirectInput registration will be skipped until you remove "
            L"the conflicting entry and re-run LibreWheel Setup or Register-LibreWheel.ps1.";
        confirm.cButtons = static_cast<UINT>(std::size(confirm_buttons));
        confirm.pButtons = confirm_buttons;
        confirm.nDefaultButton = btn_go_back;
        confirm.pfCallback = task_dialog_callback;

        int confirmed = btn_go_back;
        if (SUCCEEDED(TaskDialogIndirect(&confirm, &confirmed, nullptr, nullptr)) && confirmed == btn_keep_confirm) {
            return ConflictChoice::keep_files_only;
        }
    }
}

int run_install(bool silent, bool silent_remove_logitech, bool silent_launch) {
    const auto install_dir = default_install_dir();
    const auto config_dir = default_config_dir();
    const auto backup_dir = config_dir / L"backups";
    const bool upgrading = std::filesystem::exists(install_dir / L"LibreWheelTray.exe");

    if (!silent) {
        const std::wstring heading = std::wstring(upgrading ? L"Update LibreWheel " : L"Install LibreWheel ") + version_string();
        const std::wstring body =
            L"LibreWheel provides native DirectInput force feedback for the Logitech G29 (PS3 native mode) "
            L"without Logitech G HUB or kernel drivers.\n\n"
            L"Install folder:\n  " + install_dir.wstring() + L"\n\n"
            L"Settings folder (preserved on upgrades and uninstall):\n  " + config_dir.wstring();
        const TASKDIALOG_BUTTON buttons[]{
            {btn_install, upgrading ? L"Update" : L"Install"},
        };
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hInstance = GetModuleHandleW(nullptr);
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        config.pszWindowTitle = app_title;
        config.pszMainIcon = MAKEINTRESOURCEW(IDI_LIBREWHEEL);
        config.pszMainInstruction = heading.c_str();
        config.pszContent = body.c_str();
        config.cButtons = 1;
        config.pButtons = buttons;
        config.nDefaultButton = btn_install;
        config.pfCallback = task_dialog_callback;

        int choice = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr)) || choice != btn_install) {
            return 0;
        }
    }

    const auto conflicts = lw::win::detect_oem_conflicts();
    bool remove_conflicts = false;
    bool skip_registration = false;
    if (!conflicts.empty()) {
        if (silent) {
            remove_conflicts = silent_remove_logitech;
            skip_registration = !silent_remove_logitech;
        } else {
            const auto decision = prompt_conflict_choice(conflicts);
            if (decision == ConflictChoice::cancel) {
                return 0;
            }
            remove_conflicts = (decision == ConflictChoice::remove_and_register);
            skip_registration = (decision == ConflictChoice::keep_files_only);
        }
    }

    if (!skip_registration) {
        lw::win::install_or_switch_registration({}, {}, true, remove_conflicts);
    }

    const auto entries = load_embedded_payload();
    stop_running_tray_and_driver();

    std::vector<std::filesystem::path> backups_created;
    if (remove_conflicts) {
        std::filesystem::create_directories(backup_dir);
        if (lw::win::has_oem_conflict(false)) {
            if (auto path = lw::win::backup_and_remove_conflict(false, backup_dir)) {
                backups_created.push_back(std::move(*path));
            }
        }
        if (lw::win::has_oem_conflict(true)) {
            if (lw::win::is_process_elevated()) {
                if (auto path = lw::win::backup_and_remove_conflict(true, backup_dir)) {
                    backups_created.push_back(std::move(*path));
                }
            } else {
                run_elevated_hklm_removal(backup_dir);
            }
        }
    }

    std::size_t installed_bytes = 0;
    for (const auto& entry : entries) {
        const auto relative = std::filesystem::path(widen(entry.relative_path));
        if (entry.target == lw::win::PayloadTarget::config_if_missing) {
            const auto target = config_dir / relative;
            if (!std::filesystem::exists(target)) {
                write_file_atomic(target, entry.data, silent);
            }
        } else {
            const auto target = install_dir / relative;
            write_file_atomic(target, entry.data, silent);
            installed_bytes += entry.data.size();
        }
    }

    // Copy this executable as Uninstall.exe so Windows Installed Apps can cleanly remove LibreWheel.
    const auto self = current_executable_path();
    const auto uninstaller = install_dir / L"Uninstall.exe";
    if (!equals_icase(self.wstring(), uninstaller.wstring())) {
        std::ifstream self_stream(self, std::ios::binary | std::ios::ate);
        const auto self_size = self_stream.tellg();
        if (self_stream && self_size > 0) {
            std::vector<std::uint8_t> self_bytes(static_cast<std::size_t>(self_size));
            self_stream.seekg(0).read(reinterpret_cast<char*>(self_bytes.data()), self_size);
            write_file_atomic(uninstaller, self_bytes, silent);
            installed_bytes += self_bytes.size();
        }
    }

    const auto dll64 = install_dir / L"x64" / L"LibreWheelFF.dll";
    const auto dll32 = install_dir / L"x86" / L"LibreWheelFF.dll";
    if (!skip_registration) {
        lw::win::install_or_switch_registration(dll64, dll32, false, false);
    }

    const auto tray_exe = install_dir / L"LibreWheelTray.exe";
    if (const auto shortcut = start_menu_shortcut_path()) {
        create_shortcut(*shortcut, tray_exe, install_dir);
    }
    write_uninstall_entry(install_dir, installed_bytes);

    if (silent) {
        if (silent_launch) {
            ShellExecuteW(nullptr, L"open", tray_exe.c_str(), nullptr, install_dir.c_str(), SW_SHOWNORMAL);
        }
        return 0;
    }

    std::wstring summary;
    if (skip_registration) {
        summary =
            L"LibreWheel files were installed to:\n  " + install_dir.wstring() +
            L"\n\nDirectInput registration was skipped because the existing Logitech/OEM G29 registration was kept. "
            L"LibreWheel force feedback will NOT work until that entry is removed and LibreWheel Setup "
            L"(or Register-LibreWheel.ps1) is run again.";
    } else {
        summary = L"LibreWheel " + version_string() + L" was installed and registered for 64-bit and 32-bit DirectInput games.";
        if (remove_conflicts) {
            summary += L"\n\nRemoved conflicting Logitech/OEM registration (backup saved in " + backup_dir.wstring() + L").";
        }
    }

    BOOL launch_tray = !skip_registration ? TRUE : FALSE;
    TASKDIALOGCONFIG done{};
    done.cbSize = sizeof(done);
    done.hInstance = GetModuleHandleW(nullptr);
    done.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT |
                   (launch_tray ? TDF_VERIFICATION_FLAG_CHECKED : 0);
    done.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    done.pszWindowTitle = app_title;
    done.pszMainIcon = skip_registration ? TD_WARNING_ICON : MAKEINTRESOURCEW(IDI_LIBREWHEEL);
    done.pszMainInstruction = skip_registration ? L"Installed without DirectInput registration"
                                                : L"Installation complete";
    done.pszContent = summary.c_str();
    done.pszVerificationText = L"Launch LibreWheel now";
    done.pfCallback = task_dialog_callback;
    TaskDialogIndirect(&done, nullptr, nullptr, &launch_tray);

    if (launch_tray) {
        ShellExecuteW(nullptr, L"open", tray_exe.c_str(), nullptr, install_dir.c_str(), SW_SHOWNORMAL);
    }
    return 0;
}

void remove_startup_if_owned(const std::filesystem::path& install_dir) noexcept {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, run_key, run_value, RRF_RT_REG_SZ,
                     nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        return;
    }
    std::wstring configured(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, run_key, run_value, RRF_RT_REG_SZ,
                     nullptr, configured.data(), &bytes) != ERROR_SUCCESS) {
        return;
    }
    configured.resize(wcsnlen(configured.c_str(), configured.size()));
    const std::wstring expected = L"\"" + (install_dir / L"LibreWheelTray.exe").wstring() + L"\"";
    if (equals_icase(configured, expected)) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, run_key, run_value);
    }
}

int perform_uninstall(const std::filesystem::path& install_dir, bool silent) {
    if (!silent) {
        const TASKDIALOG_BUTTON buttons[]{
            {btn_uninstall, L"Uninstall"},
        };
        const std::wstring body =
            L"This will remove LibreWheel from:\n  " + install_dir.wstring() +
            L"\n\nand remove LibreWheel's per-user DirectInput registration.\n\n"
            L"Your settings, LUT curves, and registry backups in %LOCALAPPDATA%\\LibreWheel will be kept.";
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hInstance = GetModuleHandleW(nullptr);
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        config.pszWindowTitle = L"Uninstall LibreWheel";
        config.pszMainIcon = MAKEINTRESOURCEW(IDI_LIBREWHEEL);
        config.pszMainInstruction = L"Remove LibreWheel?";
        config.pszContent = body.c_str();
        config.cButtons = 1;
        config.pButtons = buttons;
        config.nDefaultButton = btn_uninstall;
        config.pfCallback = task_dialog_callback;

        int choice = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&config, &choice, nullptr, nullptr)) || choice != btn_uninstall) {
            return 0;
        }
    }

    stop_running_tray_and_driver();
    lw::win::remove_registration(install_dir / L"x64" / L"LibreWheelFF.dll",
                                 install_dir / L"x86" / L"LibreWheelFF.dll");
    remove_startup_if_owned(install_dir);
    if (const auto shortcut = start_menu_shortcut_path()) {
        std::error_code ec;
        std::filesystem::remove(*shortcut, ec);
    }
    RegDeleteKeyExW(HKEY_CURRENT_USER, uninstall_key, KEY_WOW64_64KEY, 0);

    std::error_code remove_err;
    std::filesystem::remove_all(install_dir, remove_err);
    if (remove_err && std::filesystem::exists(install_dir)) {
        // Schedule any still-locked DLLs (e.g., if a game was left open) for removal on reboot.
        for (const auto& entry : std::filesystem::recursive_directory_iterator(install_dir, remove_err)) {
            if (entry.is_regular_file()) {
                MoveFileExW(entry.path().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        }
        MoveFileExW(install_dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    show_message(L"Uninstall LibreWheel", L"LibreWheel has been removed",
                 L"LibreWheel binaries and DirectInput registration were removed.\n\n"
                 L"User settings and backups in %LOCALAPPDATA%\\LibreWheel were preserved.",
                 TD_INFORMATION_ICON, silent);
    return 0;
}

int launch_temp_uninstaller(const std::filesystem::path& install_dir, bool silent) {
    const auto self = current_executable_path();
    wchar_t temp_dir[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp_dir)) {
        return perform_uninstall(install_dir, silent);
    }
    const auto temp_exe = std::filesystem::path(temp_dir) /
        (L"LibreWheel-Uninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
    std::error_code ec;
    if (!std::filesystem::copy_file(self, temp_exe, std::filesystem::copy_options::overwrite_existing, ec)) {
        return perform_uninstall(install_dir, silent);
    }
    std::wstring cmd = L"\"" + temp_exe.wstring() + L"\" --uninstall-from \"" + install_dir.wstring() +
                       L"\" --wait-pid " + std::to_wstring(GetCurrentProcessId());
    if (silent) {
        cmd += L" --silent";
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(temp_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, temp_dir, &si, &pi)) {
        DeleteFileW(temp_exe.c_str());
        return perform_uninstall(install_dir, silent);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    struct FreeArgs {
        wchar_t** ptr;
        ~FreeArgs() { if (ptr) LocalFree(ptr); }
    } free_args{argv};

    bool silent = false;
    bool remove_logitech = false;
    bool launch = false;
    bool uninstall = false;
    std::filesystem::path uninstall_from;
    std::filesystem::path remove_hklm_backup;
    DWORD wait_pid = 0;

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (equals_icase(arg, L"--silent") || equals_icase(arg, L"/silent") || equals_icase(arg, L"/verysilent")) {
            silent = true;
        } else if (equals_icase(arg, L"--remove-logitech") || equals_icase(arg, L"/removelogitech")) {
            remove_logitech = true;
        } else if (equals_icase(arg, L"--launch")) {
            launch = true;
        } else if (equals_icase(arg, L"--uninstall")) {
            uninstall = true;
        } else if (equals_icase(arg, L"--uninstall-from") && i + 1 < argc) {
            uninstall_from = argv[++i];
        } else if (equals_icase(arg, L"--wait-pid") && i + 1 < argc) {
            wait_pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (equals_icase(arg, L"--remove-hklm") && i + 1 < argc) {
            remove_hklm_backup = argv[++i];
        }
    }

    if (!remove_hklm_backup.empty()) {
        try {
            lw::win::backup_and_remove_conflict(true, remove_hklm_backup);
            return 0;
        } catch (...) {
            return 1;
        }
    }

    const HRESULT com_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct UninitCom {
        bool active;
        ~UninitCom() { if (active) CoUninitialize(); }
    } uninit_com{SUCCEEDED(com_init)};

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    init_dark_mode_theme();

    try {
        if (!uninstall_from.empty()) {
            if (wait_pid) {
                if (const auto parent = lw::win::handle(OpenProcess(SYNCHRONIZE, FALSE, wait_pid))) {
                    WaitForSingleObject(parent.get(), 5000);
                }
            }
            const int rc = perform_uninstall(uninstall_from, silent);
            MoveFileExW(current_executable_path().c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            return rc;
        }
        if (uninstall) {
            const auto self = current_executable_path();
            const auto dir = equals_icase(self.filename().wstring(), L"Uninstall.exe")
                ? self.parent_path()
                : default_install_dir();
            if (equals_icase(self.parent_path().wstring(), dir.wstring())) {
                return launch_temp_uninstaller(dir, silent);
            }
            return perform_uninstall(dir, silent);
        }
        return run_install(silent, remove_logitech, launch);
    } catch (const std::exception& error) {
        show_message(app_title, L"LibreWheel Setup could not complete", widen(error.what()),
                     TD_ERROR_ICON, silent);
        return 1;
    }
}
