// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>
#include <dinput.h>
#include <dinputd.h>

#include <chrono>

namespace lw { struct Effect; }

namespace lw::win {

inline constexpr GUID driver_class{
    0x00eb836b, 0x81cb, 0x4d34, {0xb0, 0xdf, 0x16, 0x20, 0x2a, 0x83, 0xbd, 0x4a}
};

constexpr std::chrono::microseconds next_sample_deadline(
    std::chrono::microseconds current, std::chrono::microseconds previous,
    std::chrono::microseconds period, bool was_active) noexcept {
    if (!was_active) {
        return current + period;
    }
    if (current < previous) {
        return previous;
    }
    const auto next = previous + period;
    return next <= current ? current + period : next;
}

HRESULT translate_effect(DWORD type, const DIEFFECT* input, Effect& effect) noexcept;

} // namespace lw::win

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** result);
extern "C" HRESULT WINAPI DllCanUnloadNow();
