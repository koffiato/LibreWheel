// SPDX-License-Identifier: GPL-3.0-or-later
#include "driver.hpp"
#include "hid.hpp"
#include "settings.hpp"

#include <cfgmgr32.h>
#include <algorithm>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>

namespace {

using namespace lw::win;
HANDLE stop_monitor = nullptr;

BOOL WINAPI console_stop(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        SetEvent(stop_monitor);
        return TRUE;
    }
    return FALSE;
}

DWORD CALLBACK interface_changed(HCMNOTIFICATION, void* event, CM_NOTIFY_ACTION,
                                 PCM_NOTIFY_EVENT_DATA, DWORD) {
    SetEvent(event);
    return ERROR_SUCCESS;
}

void describe(const DeviceInfo& device) {
    std::wcout << L"046d:c24f rev " << std::hex << device.revision << std::dec
               << L"  input=" << device.input_bytes << L"  output=" << device.output_bytes
               << L"  classic=" << (device.classic_output ? L"yes" : L"no")
               << L"\n" << device.path << L"\n";
}

int monitor() {
    const auto stop = handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const auto arrival = handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!stop || !arrival) {
        throw std::runtime_error("Unable to create monitor events");
    }
    stop_monitor = stop.get();
    if (!SetConsoleCtrlHandler(console_stop, TRUE)) {
        throw std::runtime_error("Unable to register the console exit handler");
    }
    struct RestoreConsole {
        ~RestoreConsole() { SetConsoleCtrlHandler(console_stop, FALSE); }
    } restore;

    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = hid_interface_guid();
    struct Notification {
        HCMNOTIFICATION token = nullptr;
        ~Notification() { if (token) CM_Unregister_Notification(token); }
    } notification;
    if (CM_Register_Notification(&filter, arrival.get(), interface_changed, &notification.token) != CR_SUCCESS) {
        throw std::runtime_error("Unable to register HID hotplug notification");
    }
    std::unique_ptr<HidInput> input;
    std::wcout << L"Monitoring the native G29 joystick collection. Ctrl+C exits.\n";
    SetEvent(arrival.get());
    while (true) {
        const HANDLE events[]{stop.get(), arrival.get(), input ? input->ready() : nullptr};
        const auto ready = WaitForMultipleObjects(input ? 3 : 2, events, FALSE, INFINITE);
        if (ready == WAIT_OBJECT_0) {
            return 0;
        }
        if (ready == WAIT_OBJECT_0 + 1 && !input) {
            const auto candidates = discover();
            if (candidates.size() != 1) {
                std::wcout << (candidates.empty() ? L"Waiting for a G29 in c24f mode.\n"
                                                 : L"Connect one G29 to monitor it.\n");
                continue;
            }
            try {
                auto attached = std::make_unique<HidInput>(candidates.front().path);
                attached->begin();
                describe(attached->info());
                input = std::move(attached);
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
            }
        } else if (ready == WAIT_OBJECT_0 + 2) {
            try {
                const auto state = input->take();
                input->begin();
                if (state) {
                    std::wcout << L"\rWheel " << std::setw(5) << state->steering
                               << L"  accelerator " << std::setw(3) << state->accelerator
                               << L"  brake " << std::setw(3) << state->brake
                               << L"  clutch " << std::setw(3) << state->clutch
                               << L"  hat " << std::setw(2) << state->hat
                               << L"  buttons 0x" << std::hex << std::setw(7) << state->buttons
                               << std::dec << L"  " << std::flush;
                }
            } catch (const std::system_error& error) {
                std::wcout << L"\n";
                std::cerr << error.what() << '\n';
                input.reset();
                // Re-enumerate on a new notification. A malformed report should not cause a reopen loop.
                if (error.code().value() != ERROR_INVALID_DATA) {
                    SetEvent(arrival.get());
                }
            }
        } else if (ready == WAIT_FAILED) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Wait for HID");
        }
    }
}

template<class T>
struct ReleaseCom { void operator()(T* pointer) const noexcept { pointer->Release(); } };

constexpr auto g29_product = MAKELONG(lw::g29::vendor_id, lw::g29::product_id);
using DirectInput = std::unique_ptr<IDirectInput8W, ReleaseCom<IDirectInput8W>>;
using DirectDevice = std::unique_ptr<IDirectInputDevice8W, ReleaseCom<IDirectInputDevice8W>>;

void checked(HRESULT status, const char* operation) {
    if (FAILED(status)) {
        std::cerr << operation << " failed (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(status) << ")\n";
        throw std::runtime_error("DirectInput operation failed");
    }
}

DirectInput create_directinput() {
    IDirectInput8W* raw = nullptr;
    checked(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                              reinterpret_cast<void**>(&raw), nullptr), "Initialize DirectInput 8");
    return DirectInput(raw);
}

BOOL CALLBACK describe_axis(const DIDEVICEOBJECTINSTANCEW* axis, void*) {
    std::wcout << L"  " << axis->tszName << L"  actuator="
               << ((axis->dwFlags & DIDOI_FFACTUATOR) ? L"yes" : L"no") << L"\n";
    return DIENUM_CONTINUE;
}

BOOL CALLBACK describe_effect(const DIEFFECTINFOW* effect, void*) {
    std::wcout << L"  " << effect->tszName << L"\n";
    return DIENUM_CONTINUE;
}

struct Probe {
    IDirectInput8W* api;
    bool found = false;
    bool failed = false;
};

BOOL CALLBACK describe_device(const DIDEVICEINSTANCEW* details, void* context) {
    auto& probe = *static_cast<Probe*>(context);
    if (details->guidProduct.Data1 != static_cast<DWORD>(g29_product)) {
        return DIENUM_CONTINUE;
    }
    probe.found = true;
    IDirectInputDevice8W* raw = nullptr;
    HRESULT result = probe.api->CreateDevice(details->guidInstance, &raw, nullptr);
    if (SUCCEEDED(result)) {
        const DirectDevice device(raw);
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        result = device->GetCapabilities(&caps);
        if (SUCCEEDED(result)) {
            std::wcout << details->tszProductName << L"  axes=" << caps.dwAxes << L"  buttons=" << caps.dwButtons
                       << L"  POVs=" << caps.dwPOVs << L"  force feedback="
                       << ((caps.dwFlags & DIDC_FORCEFEEDBACK) ? L"yes" : L"no")
                       << L"  LibreWheel=" << (details->guidFFDriver == driver_class ? L"yes" : L"no") << L"\n";
            device->EnumObjects(describe_axis, nullptr, DIDFT_AXIS);
            device->EnumEffects(describe_effect, nullptr, DIEFT_ALL);
        }
    }
    if (FAILED(result)) {
        probe.failed = true;
        std::cerr << "DirectInput device probe failed (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(result) << ")\n";
    }
    return DIENUM_CONTINUE;
}

int directinput() {
    const auto api = create_directinput();
    Probe probe{api.get()};
    if (FAILED(api->EnumDevices(DI8DEVCLASS_GAMECTRL, describe_device, &probe, DIEDFL_ATTACHEDONLY))) {
        throw std::runtime_error("Unable to enumerate DirectInput controllers");
    }
    if (!probe.found) {
        std::wcout << L"No G29 found in DirectInput.\n";
        return 2;
    }
    return probe.failed ? 1 : 0;
}

struct Selection {
    GUID instance{};
    unsigned found = 0;
};

BOOL CALLBACK select_librewheel(const DIDEVICEINSTANCEW* details, void* context) {
    if (details->guidProduct.Data1 == static_cast<DWORD>(g29_product) &&
        details->guidFFDriver == driver_class) {
        auto& selection = *static_cast<Selection*>(context);
        selection.instance = details->guidInstance;
        ++selection.found;
    }
    return DIENUM_CONTINUE;
}

int pulse(bool right, unsigned percent) {
    using namespace std::chrono_literals;
    const auto api = create_directinput();
    Selection selection;
    checked(api->EnumDevices(DI8DEVCLASS_GAMECTRL, select_librewheel, &selection, DIEDFL_ATTACHEDONLY |
                             DIEDFL_FORCEFEEDBACK), "Find LibreWheel force-feedback device");
    if (selection.found != 1) {
        throw std::runtime_error("Pulse requires exactly one G29 with the LibreWheel FFB class");
    }
    const auto destroy = [](HWND window) { DestroyWindow(window); };
    const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(destroy)> window(
        CreateWindowExW(0, L"STATIC", L"LibreWheel pulse", WS_OVERLAPPED,
                        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
    if (!window) {
        throw std::runtime_error("Unable to create the DirectInput test window");
    }
    IDirectInputDevice8W* raw = nullptr;
    checked(api->CreateDevice(selection.instance, &raw, nullptr), "Create physical G29 device");
    const DirectDevice device(raw);
    checked(device->SetDataFormat(&c_dfDIJoystick2), "Set joystick data format");
    checked(device->SetCooperativeLevel(window.get(), DISCL_EXCLUSIVE | DISCL_BACKGROUND),
            "Set exclusive cooperative level");
    checked(device->Acquire(), "Acquire the physical G29");

    DWORD axis = DIJOFS_X;
    LONG direction = 0;
    const auto nominal = static_cast<LONG>(percent * DI_FFNOMINALMAX / 100);
    // A rightward pull has its DirectInput source on the left.
    DICONSTANTFORCE constant{right ? -nominal : nominal};
    DIEFFECT description{};
    description.dwSize = sizeof(description);
    description.dwFlags = DIEFF_OBJECTOFFSETS | DIEFF_CARTESIAN;
    description.dwDuration = 1'000'000;
    description.dwGain = DI_FFNOMINALMAX;
    description.dwTriggerButton = DIEB_NOTRIGGER;
    description.cAxes = 1;
    description.rgdwAxes = &axis;
    description.rglDirection = &direction;
    description.cbTypeSpecificParams = sizeof(constant);
    description.lpvTypeSpecificParams = &constant;
    IDirectInputEffect* effect_raw = nullptr;
    checked(device->CreateEffect(GUID_ConstantForce, &description, &effect_raw, nullptr), "Create constant force");
    const std::unique_ptr<IDirectInputEffect, ReleaseCom<IDirectInputEffect>> effect(effect_raw);
    checked(effect->Start(1, DIES_SOLO), "Start constant force");
    DWORD state = 0;
    checked(effect->GetEffectStatus(&state), "Read running effect status");
    std::this_thread::sleep_for(1100ms);
    DWORD after = 0;
    checked(effect->GetEffectStatus(&after), "Read expired effect status");
    checked(effect->Stop(), "Stop constant force");
    checked(device->Unacquire(), "Release physical G29");
    if (!(state & DIEGES_PLAYING) || (after & DIEGES_PLAYING)) {
        throw std::runtime_error("DirectInput playback status did not match the finite pulse");
    }
    std::wcout << L"The " << percent << L"% DirectInput pulse started, expired, and released successfully.\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2) {
            const std::wstring_view command(argv[1]);
            if (command == L"list") {
                const auto candidates = discover();
                for (const auto& device : candidates) {
                    describe(device);
                }
                if (candidates.empty()) {
                    std::wcout << L"No connected G29 native joystick collection (046d:c24f).\n";
                    return 2;
                }
                return 0;
            }
            if (command == L"native") {
                const auto device = initialize_native();
                if (!device) {
                    std::wcout << L"No G29 in native mode or supported PS3 compatibility mode found.\n";
                    return 2;
                }
                describe(*device);
                return 0;
            }
            if (command == L"monitor") {
                return monitor();
            }
            if (command == L"directinput") {
                return directinput();
            }
            if (command == L"settings") {
                const auto path = settings_path();
                const auto settings = lw::load_settings(path);
                std::wcout << path.c_str() << L"\nGain " << settings.gain * 100 << L"%  LUT "
                           << (settings.lut ? L"enabled" : L"disabled") << L"  diagnostics "
                           << (settings.diagnostics ? L"enabled" : L"disabled") << L"  FFB logging "
                           << (settings.log_ffb ? L"enabled" : L"disabled") << L"\n";
                if (settings.range) {
                    std::wcout << L"Configured range " << *settings.range << L" degrees\n";
                }
                return 0;
            }
        }
        if ((argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"pulse") {
            const std::wstring_view direction(argv[2]);
            if (direction == L"left" || direction == L"right") {
                unsigned percent = 5;
                if (argc == 4) {
                    const std::wstring_view value(argv[3]);
                    if (value.empty() || value.size() > 3 ||
                        !std::ranges::all_of(value, [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) {
                        throw std::runtime_error("Pulse percentage must be an integer from 1 to 100");
                    }
                    percent = static_cast<unsigned>(std::wcstol(argv[3], nullptr, 10));
                    if (percent == 0 || percent > 100) {
                        throw std::runtime_error("Pulse percentage must be from 1 to 100");
                    }
                }
                return pulse(direction == L"right", percent);
            }
        }
        std::wcout << L"Usage: librewheel {list|native|monitor|directinput|settings|pulse <left|right> [percent]}\n";
        return argc == 1 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
