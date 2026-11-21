// SPDX-License-Identifier: GPL-3.0-or-later
#include "hid.hpp"
#include "settings.hpp"
#include "tray_resource.h"

#include <cfgmgr32.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

namespace {

constexpr wchar_t window_class[] = L"LibreWheel.Tray.Window";
constexpr wchar_t instance_mutex[] = L"Local\\LibreWheel.Tray";
constexpr wchar_t run_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t run_value[] = L"LibreWheelTray";
constexpr UINT icon_id = 1;
constexpr UINT_PTR icon_retry = 1;
constexpr UINT tray_callback = WM_APP + 1;
constexpr UINT show_menu = WM_APP + 2;
constexpr UINT startup_item = 1;
constexpr UINT quit_item = 2;
constexpr UINT first_lut_item = 3;

void show_error(HWND window, const wchar_t* message) {
    MessageBoxW(window, message, L"LibreWheel", MB_OK | MB_ICONERROR);
}

bool starts_with_windows() {
    DWORD bytes = 0;
    return RegGetValueW(HKEY_CURRENT_USER, run_key, run_value, RRF_RT_REG_SZ,
                        nullptr, nullptr, &bytes) == ERROR_SUCCESS;
}

bool set_start_with_windows(bool enabled) {
    if (!enabled) {
        const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, run_key, run_value);
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }

    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) {
        return false;
    }
    executable.resize(length);
    const auto command = L'"' + executable + L'"';
    // Windows documents a 260-character limit for commands in the Run key.
    if (command.size() > MAX_PATH) {
        return false;
    }
    const auto bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    return RegSetKeyValueW(HKEY_CURRENT_USER, run_key, run_value, REG_SZ, command.c_str(), bytes) == ERROR_SUCCESS;
}

const wchar_t* detected_wheel() {
    try {
        if (!lw::win::discover().empty()) {
            return L"Logitech G29 Driving Force (native mode)";
        }
        if (lw::win::compatibility_connected()) {
            return L"Logitech G29 (compatibility mode)";
        }
        return L"No Logitech G29 detected";
    } catch (const std::exception&) {
        return L"Wheel detection unavailable";
    }
}

void switch_to_native(HANDLE stop, unsigned retries) noexcept {
    try {
        for (unsigned attempt = 0; ; ++attempt) {
            if (lw::win::initialize_native() || attempt == retries || WaitForSingleObject(stop, 200) != WAIT_TIMEOUT) {
                return;
            }
        }
    } catch (const std::exception& error) {
        OutputDebugStringA("LibreWheel native mode switch: ");
        OutputDebugStringA(error.what());
        OutputDebugStringA("\n");
    }
}

DWORD CALLBACK wheel_arrived(HCMNOTIFICATION, void* arrival, CM_NOTIFY_ACTION action,
                            PCM_NOTIFY_EVENT_DATA data, DWORD) {
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL &&
        lw::win::is_compatibility_interface(data->u.DeviceInterface.SymbolicLink)) {
        SetEvent(arrival);
    }
    return ERROR_SUCCESS;
}

void watch_wheel(std::stop_token token, HANDLE stop, HANDLE arrival) {
    const std::stop_callback signal_stop(token, [stop] { SetEvent(stop); });
    const HANDLE events[]{stop, arrival};
    if (!token.stop_requested()) {
        switch_to_native(stop, 0);
    }
    while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        // A new interface can be visible before its HID descriptor can be opened.
        switch_to_native(stop, 4);
    }
}

std::string read_settings_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file && !std::filesystem::exists(path)) {
        return {};
    }
    const auto size = file.tellg();
    if (size < 0) {
        throw std::runtime_error("Unable to read settings file");
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!file.seekg(0).read(text.data(), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("Unable to read settings file");
    }
    return text;
}

bool lut_checked(const lw::LutSelection& selection, const std::filesystem::path& file) {
    if (!selection.enabled || selection.file.empty()) {
        return false;
    }
    const auto configured = (file.parent_path() / selection.file).lexically_normal();
    return CompareStringOrdinal(configured.c_str(), -1, file.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::vector<std::filesystem::path> append_lut_menu(HMENU menu) {
    const auto submenu = CreatePopupMenu();
    if (!submenu) {
        return {};
    }
    if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), L"&LUT File")) {
        DestroyMenu(submenu);
        return {};
    }

    std::vector<std::filesystem::path> files;
    try {
        const auto settings = lw::win::settings_path();
        std::istringstream ini(read_settings_text(settings));
        const auto selection = lw::read_lut_selection(ini);
        if (std::filesystem::exists(settings.parent_path())) {
            for (const auto& entry : std::filesystem::directory_iterator(settings.parent_path())) {
                const auto suffix = entry.path().extension();
                if ((CompareStringOrdinal(suffix.c_str(), -1, L".csv", -1, TRUE) == CSTR_EQUAL ||
                     CompareStringOrdinal(suffix.c_str(), -1, L".lut", -1, TRUE) == CSTR_EQUAL) &&
                    entry.is_regular_file()) {
                    files.push_back(entry.path().lexically_normal());
                }
            }
        }
        std::ranges::sort(files, [](const auto& left, const auto& right) {
            return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
        for (std::size_t index = 0; index < files.size(); ++index) {
            std::wstring label;
            for (const wchar_t character : files[index].filename().wstring()) {
                label += character;
                if (character == L'&') {
                    label += character;
                }
            }
            AppendMenuW(submenu, MF_STRING | (lut_checked(selection, files[index]) ? MF_CHECKED : MF_UNCHECKED),
                        first_lut_item + index, label.c_str());
        }
        if (files.empty()) {
            AppendMenuW(submenu, MF_GRAYED, 0, L"No LUT files found");
        }
    } catch (const std::exception&) {
        AppendMenuW(submenu, MF_GRAYED, 0, L"LUT files unavailable");
        files.clear();
    }
    return files;
}

void toggle_lut(const std::filesystem::path& file) {
    const auto path = lw::win::settings_path();
    const auto original = read_settings_text(path);
    std::istringstream ini(original);
    auto selection = lw::read_lut_selection(ini);
    selection.enabled = !lut_checked(selection, file);
    if (selection.enabled) {
        std::ifstream curve(file);
        if (!curve) {
            throw std::runtime_error("Unable to open LUT file");
        }
        lw::Lut::read(curve);
        selection.file = file.filename();
        if (selection.file.native().starts_with(L' ')) {
            selection.file = std::filesystem::path(L".") / selection.file;
        }
    }

    const auto updated = lw::write_lut_selection(original, selection);
    wchar_t temporary[MAX_PATH];
    if (!GetTempFileNameW(path.parent_path().c_str(), L"LUT", 0, temporary)) {
        throw std::runtime_error("Unable to create a temporary settings file");
    }
    try {
        std::ofstream output(std::filesystem::path(temporary), std::ios::binary);
        output << updated;
        output.close();
        if (!output || read_settings_text(path) != original ||
            !MoveFileExW(temporary, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Unable to replace settings file");
        }
    } catch (...) {
        DeleteFileW(temporary);
        throw;
    }
}

NOTIFYICONDATAW notification(HWND window) {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window;
    icon.uID = icon_id;
    return icon;
}

void add_icon(HWND window) {
    auto icon = notification(window);
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = tray_callback;
    icon.hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICONSM));
    wcscpy_s(icon.szTip, L"LibreWheel");
    if (Shell_NotifyIconW(NIM_ADD, &icon)) {
        icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon);
        KillTimer(window, icon_retry);
    } else {
        SetTimer(window, icon_retry, 1000, nullptr);
    }
}

void open_menu(HWND window, POINT anchor) {
    static bool menu_open = false;
    if (menu_open) {
        return;
    }
    const auto menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    const bool startup_enabled = starts_with_windows();
    AppendMenuW(menu, MF_GRAYED, 0, detected_wheel());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const auto lut_files = append_lut_menu(menu);
    AppendMenuW(menu, MF_STRING | (startup_enabled ? MF_CHECKED : MF_UNCHECKED),
                startup_item, L"Start with &Windows");
    AppendMenuW(menu, MF_STRING, quit_item, L"&Quit");

    if (anchor.x == -1 && anchor.y == -1) {
        const NOTIFYICONIDENTIFIER identifier{sizeof(NOTIFYICONIDENTIFIER), window, icon_id, {}};
        RECT bounds{};
        if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &bounds))) {
            anchor = {bounds.left, bounds.top};
        } else {
            GetCursorPos(&anchor);
        }
    }
    SetForegroundWindow(window);
    menu_open = true;
    const UINT choice = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                       anchor.x, anchor.y, 0, window, nullptr);
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (choice == quit_item) {
        menu_open = false;
        DestroyWindow(window);
        return;
    }
    if (choice == startup_item && !set_start_with_windows(!startup_enabled)) {
        show_error(window, L"LibreWheel could not change the Start with Windows setting.");
    }
    if (choice >= first_lut_item && choice - first_lut_item < lut_files.size()) {
        try {
            toggle_lut(lut_files[choice - first_lut_item]);
        } catch (const std::exception&) {
            show_error(window, L"LibreWheel could not change the LUT. Check the curve file and settings.ini.");
        }
    }
    auto icon = notification(window);
    Shell_NotifyIconW(NIM_SETFOCUS, &icon);
    menu_open = false;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    static const UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (message == taskbar_created) {
        add_icon(window);
        return 0;
    }

    switch (message) {
    case tray_callback:
        switch (LOWORD(lparam)) {
        case WM_CONTEXTMENU:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            open_menu(window, {GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)});
            break;
        case WM_RBUTTONUP:
            if (HIWORD(lparam) == 0) { // Legacy callback if NIM_SETVERSION failed.
                POINT cursor{};
                GetCursorPos(&cursor);
                open_menu(window, cursor);
            }
            break;
        }
        return 0;
    case show_menu:
        open_menu(window, {-1, -1});
        return 0;
    case WM_TIMER:
        if (wparam == icon_retry) {
            add_icon(window);
        }
        return 0;
    case WM_DESTROY: {
        auto icon = notification(window);
        Shell_NotifyIconW(NIM_DELETE, &icon);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const auto mutex = lw::win::handle(CreateMutexW(nullptr, FALSE, instance_mutex));
    const DWORD mutex_status = GetLastError();
    if (!mutex) {
        show_error(nullptr, L"LibreWheel could not start.");
        return 1;
    }
    if (mutex_status == ERROR_ALREADY_EXISTS) {
        if (const auto window = FindWindowW(window_class, nullptr)) {
            DWORD process_id = 0;
            GetWindowThreadProcessId(window, &process_id);
            AllowSetForegroundWindow(process_id);
            PostMessageW(window, show_menu, 0, 0);
        }
        return 0;
    }

    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.lpfnWndProc = window_proc;
    definition.hInstance = instance;
    definition.lpszClassName = window_class;
    definition.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_LIBREWHEEL));
    definition.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_LIBREWHEEL), IMAGE_ICON,
                                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                       LR_SHARED));
    if (!RegisterClassExW(&definition)) {
        show_error(nullptr, L"LibreWheel could not register its tray window.");
        return 1;
    }
    const auto window = CreateWindowExW(0, window_class, L"LibreWheel", WS_OVERLAPPED,
                                        0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!window) {
        show_error(nullptr, L"LibreWheel could not create its tray window.");
        return 1;
    }
    add_icon(window);

    const auto stop = lw::win::handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const auto arrival = lw::win::handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = lw::win::hid_interface_guid();
    struct DeviceNotification {
        HCMNOTIFICATION token = nullptr;
        ~DeviceNotification() { if (token) CM_Unregister_Notification(token); }
    } notification;
    std::jthread watcher;
    try {
        if (stop && arrival &&
            CM_Register_Notification(&filter, arrival.get(), wheel_arrived, &notification.token) == CR_SUCCESS) {
            watcher = std::jthread(watch_wheel, stop.get(), arrival.get());
        }
    } catch (const std::system_error& error) {
        OutputDebugStringA(error.what());
    }
    if (!watcher.joinable()) {
        OutputDebugStringA("LibreWheel could not register for G29 reconnect notifications.\n");
        switch_to_native(nullptr, 0);
    }

    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}
