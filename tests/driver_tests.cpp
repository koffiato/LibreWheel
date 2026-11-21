// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows/driver.hpp"
#include "windows/hid.hpp"
#include "effects.hpp"

#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

int failures = 0;
void check(bool success, const char* description) {
    if (!success) {
        ++failures;
        std::cerr << description << '\n';
    }
}

void compatibility_arrival() {
    using lw::win::is_compatibility_interface;
    check(is_compatibility_interface(LR"(\\?\HID#VID_046D&PID_C294#7&e23ac16&0&0000#{guid})"),
          "recognize an arriving G29 compatibility interface");
    check(is_compatibility_interface(LR"(\??\hid#vid_046d&pid_c294&mi_00#instance)"),
          "recognize the NT namespace form of an interface path");
    check(!is_compatibility_interface(LR"(\\?\hid#vid_046d&pid_c24f&mi_00#instance)"),
          "native re-enumeration does not request another mode switch");
    check(!is_compatibility_interface(LR"(\\?\hid#vid_046d&pid_c2940#instance)"),
          "a partial product ID is not a compatibility arrival");
}

void polarity() {
    DWORD axis = DIDFT_ABSAXIS;
    LONG direction = 0;
    DICONSTANTFORCE constant{5000};
    DIEFFECT effect{};
    effect.dwSize = sizeof(effect);
    effect.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    effect.dwDuration = INFINITE;
    effect.dwGain = DI_FFNOMINALMAX;
    effect.cAxes = 1;
    effect.rgdwAxes = &axis;
    effect.rglDirection = &direction;
    effect.cbTypeSpecificParams = sizeof(constant);
    effect.lpvTypeSpecificParams = &constant;
    const auto force = [&] {
        lw::Effect translated;
        check(lw::win::translate_effect(0, &effect, translated) == DI_OK, "translate signed constant force");
        return lw::evaluate(translated, lw::Time{}, {});
    };
    check(force() == -0.5, "positive one-axis magnitude pushes away from +X");
    constant.lMagnitude = -5000;
    check(force() == 0.5, "negative one-axis magnitude reverses the pull");
    constant.lMagnitude = 5000;
    direction = 1;
    check(force() == -0.5, "explicit positive Cartesian source matches the implicit source");
    direction = -1;
    check(force() == 0.5, "explicit negative Cartesian source pushes toward +X");
    effect.dwFlags = DIEFF_OBJECTIDS | DIEFF_SPHERICAL;
    direction = 0;
    check(force() == -0.5, "one-axis spherical source also defaults to +X");
}

void lifecycle(IDirectInputEffectDriver& driver, wchar_t* device_path, bool physical);

bool exports(const wchar_t* path, wchar_t* device_path = nullptr) {
    const auto dll = LoadLibraryW(path);
    if (!dll) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return false;
    }
    using GetFactory = decltype(&DllGetClassObject);
    using CanUnload = decltype(&DllCanUnloadNow);
#pragma warning(suppress: 4191) // The DLL's exports are tested against their declared COM signatures.
    const auto get_factory = reinterpret_cast<GetFactory>(GetProcAddress(dll, "DllGetClassObject"));
#pragma warning(suppress: 4191)
    const auto can_unload = reinterpret_cast<CanUnload>(GetProcAddress(dll, "DllCanUnloadNow"));
    IClassFactory* factory = nullptr;
    bool success = get_factory && can_unload &&
        get_factory(lw::win::driver_class, IID_IClassFactory, reinterpret_cast<void**>(&factory)) == S_OK &&
        factory && can_unload() == S_FALSE;
    if (factory) {
        if (success && device_path) {
            IDirectInputEffectDriver* driver = nullptr;
            success = factory->CreateInstance(nullptr, IID_IDirectInputEffectDriver,
                                               reinterpret_cast<void**>(&driver)) == S_OK && driver;
            if (driver) {
                lifecycle(*driver, device_path, true);
                driver->Release();
            }
        }
        factory->Release();
        success = success && can_unload() == S_OK;
    }
    FreeLibrary(dll);
    return success;
}

void lifecycle(IDirectInputEffectDriver& driver, wchar_t* device_path, bool physical) {
    DWORD axis = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0);
    LONG direction = 0;
    DICONSTANTFORCE constant{3600};
    DIEFFECT effect{};
    effect.dwSize = sizeof(effect);
    effect.dwFlags = DIEFF_OBJECTIDS | DIEFF_CARTESIAN;
    effect.dwDuration = INFINITE;
    effect.dwGain = DI_FFNOMINALMAX;
    effect.dwTriggerButton = DIEB_NOTRIGGER;
    effect.cAxes = 1;
    effect.rgdwAxes = &axis;
    effect.rglDirection = &direction;
    effect.cbTypeSpecificParams = sizeof(constant);
    effect.lpvTypeSpecificParams = &constant;

    constexpr DWORD id = 37;
    DWORD handle = 0;
    check(driver.DownloadEffect(id, 0, &handle, &effect, 0) == DIERR_NOTEXCLUSIVEACQUIRED,
          "download requires a successful DeviceID");
    check(FAILED(driver.DeviceID(DIRECTINPUT_VERSION, id, 1, 0, nullptr)), "missing HID init rejected");
    DIHIDFFINITINFO init{sizeof(init), device_path, {}};
    const auto attach = driver.DeviceID(DIRECTINPUT_VERSION, id, 1, 0, &init);
    check(attach == DI_OK, "attach HID device");
    if (FAILED(attach)) {
        std::cerr << "DeviceID HRESULT 0x" << std::hex << static_cast<unsigned long>(attach) << '\n';
        return;
    }
    check(driver.DeviceID(DIRECTINPUT_VERSION, id + 1, 1, 0, &init) == DIERR_OTHERAPPHASPRIO,
          "a second device cannot hijack an open session");
    check(driver.DownloadEffect(id, 0, &handle, &effect, DIEP_NODOWNLOAD | DIEP_START) == DI_OK && !handle,
          "validate without downloading or starting");
    check(driver.DownloadEffect(id, 0, &handle, &effect, DIEP_START) == DI_OK && handle,
          "new started effect");
    DWORD state = 0;
    check(driver.GetEffectStatus(id, handle, &state) == DI_OK && state == DIEGES_PLAYING,
          "playing state reported");
    const auto original = handle;
    effect.cbTypeSpecificParams = 0;
    check(FAILED(driver.DownloadEffect(id, 0, &handle, &effect, 0)) && handle == original,
          "invalid update preserves original effect");
    effect.cbTypeSpecificParams = sizeof(constant);
    constant.lMagnitude = -2400;
    check(driver.DownloadEffect(id, 0, &handle, &effect, DIEP_TYPESPECIFICPARAMS | DIEP_NORESTART) == DI_OK &&
          handle == original, "download updates in place");
    check(driver.GetEffectStatus(id, handle, &state) == DI_OK && state == DIEGES_PLAYING,
          "effect update preserves running state");
    DWORD second = 0;
    check(driver.DownloadEffect(id, 0, &second, &effect, 0) == DI_OK && second != handle,
          "second downloaded effect gets distinct ID");
    check(driver.StartEffect(id, second, DIES_SOLO, 1) == DI_OK, "solo start");
    check(driver.GetEffectStatus(id, handle, &state) == DI_OK && !state, "solo stops previous effect");
    check(driver.SendForceFeedbackCommand(id, DISFFC_PAUSE) == DI_OK, "pause command");
    DIDEVICESTATE device{sizeof(device), 0, 0};
    check(driver.GetForceFeedbackState(id, &device) == DI_OK && (device.dwState & DIGFFS_PAUSED),
          "device paused status");
    check(driver.SendForceFeedbackCommand(id, DISFFC_CONTINUE) == DI_OK, "resume command");
    check(driver.SetGain(id, 5000) == DI_OK, "device gain");
    if (physical) {
        effect.dwDuration = 100000;
        check(driver.DownloadEffect(id, 0, &second, &effect, DIEP_START) == DI_OK,
              "start a finite effect with the physical HID read loop active");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        check(driver.GetEffectStatus(id, second, &state) == DI_OK && !state,
              "the physical HID read loop stays healthy and the finite effect expires");
    }
    check(driver.SendForceFeedbackCommand(id, DISFFC_SETACTUATORSOFF) == DI_OK, "disable command");
    check(driver.GetForceFeedbackState(id, &device) == DI_OK && (device.dwState & DIGFFS_ACTUATORSOFF),
          "disabled actuator status");
    check(driver.StopEffect(id, second) == DI_OK && driver.DestroyEffect(id, second) == DI_OK,
          "stop then destroy");
    check(driver.StartEffect(id, second, 0, 1) == DIERR_NOTDOWNLOADED, "destroyed handle is invalid");
    check(driver.SendForceFeedbackCommand(id, DISFFC_RESET) == DI_OK, "reset command");
    check(driver.GetForceFeedbackState(id, &device) == DI_OK && (device.dwState & DIGFFS_EMPTY),
          "reset empties effect pool");
    check(driver.DeviceID(DIRECTINPUT_VERSION, id, 0, 0, nullptr) == DI_OK,
          "device release");
    check(driver.StartEffect(id, handle, 0, 1) == DIERR_NOTEXCLUSIVEACQUIRED, "release ends session");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[2]) == L"--physical") {
        // The ABI test must remain read-only even when the provided DLL is a bench build.
        check(SetEnvironmentVariableW(L"LIBREWHEEL_BENCH_OUTPUT", L"0") != FALSE,
              "disable motor output for the physical ABI test");
        if (failures) {
            return 1;
        }
        const auto wheels = lw::win::discover();
        if (wheels.size() != 1 || !wheels.front().classic_output) {
            std::cerr << "Expected one native G29 with its classic output field\n";
            return 1;
        }
        auto device_path = wheels.front().path;
        check(exports(argv[1], device_path.data()), "physical device DLL lifecycle");
        if (!failures) {
            std::cout << "The production DLL attached to the physical G29 and completed the effect lifecycle without motor output.\n";
        }
        return failures != 0;
    }
    compatibility_arrival();
    polarity();
    check(argc == 2 && exports(argv[1]), "load COM DLL and resolve exports");
    IClassFactory* factory = nullptr;
    check(DllGetClassObject(lw::win::driver_class, IID_IClassFactory,
                            reinterpret_cast<void**>(&factory)) == S_OK, "create class factory");
    if (factory) {
        IDirectInputEffectDriver* driver = nullptr;
        check(factory->CreateInstance(nullptr, IID_IDirectInputEffectDriver,
                                      reinterpret_cast<void**>(&driver)) == S_OK, "create effect driver");
        if (driver) {
            wchar_t fixture_path[] = L"fixture";
            lifecycle(*driver, fixture_path, false);
            driver->Release();
        }
        factory->Release();
    }
    check(DllCanUnloadNow() == S_OK, "no COM references leaked");
    return failures != 0;
}
