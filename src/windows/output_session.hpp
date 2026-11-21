// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "output_lease.hpp"

#include <algorithm>

namespace lw::win {

struct OutputSignals {
    HANDLE parent;
    HANDLE changed;
    HANDLE stop;
    HANDLE ready;
};

enum class OutputStrength { limited, full };

template<class Output>
bool drive_output(Output& output, OutputSignals signals, OutputMailbox& mailbox, unsigned range,
                  OutputStrength strength) {
    constexpr std::uint8_t idle = 0x80;
    struct Neutral {
        Output& output;
        ~Neutral() {
            for (unsigned attempt = 0; attempt < 3; ++attempt) {
                if (output.send(g29::torque(idle))) {
                    return;
                }
            }
            OutputDebugStringW(L"LibreWheel: wheel did not acknowledge the stop report\n");
        }
    } neutral{output};

    const HANDLE events[]{signals.stop, signals.parent, signals.changed};
    if (WaitForMultipleObjects(2, events, FALSE, 0) != WAIT_TIMEOUT ||
        !output.send(g29::torque(idle)) || !output.send(g29::disable_centering())) {
        return false;
    }
    if (range) {
        const auto report = g29::operating_range(range);
        if (!report || !output.send(*report)) {
            return false;
        }
    }
    if (!SetEvent(signals.ready)) {
        return false;
    }

    std::uint8_t sent = idle;
    auto latest_tick = GetTickCount64();
    while (true) {
        const auto age = GetTickCount64() - latest_tick;
        const auto remaining = age < lease_timeout_ms ? lease_timeout_ms - static_cast<DWORD>(age) : 0;
        const auto wake = WaitForMultipleObjects(3, events, FALSE, remaining);
        if (wake != WAIT_OBJECT_0 + 2 && wake != WAIT_TIMEOUT) {
            return wake != WAIT_FAILED;
        }
        // A steady force may refresh the shared timestamp without signaling changed.
        const auto latest = static_cast<ULONGLONG>(InterlockedCompareExchange64(&mailbox.latest, 0, 0));
        latest_tick = latest >> 8;
        if (GetTickCount64() - latest_tick >= lease_timeout_ms) {
            return true;
        }
        const auto requested = static_cast<std::uint8_t>(latest & 0xff);
        // Limit bench builds to about 10% of the classic force-byte excursion.
        const auto force = strength == OutputStrength::limited
            ? std::clamp(requested, std::uint8_t{0x74}, std::uint8_t{0x8c})
            : requested;
        if (force != sent) {
            if (!output.send(g29::torque(force))) {
                return false;
            }
            sent = force;
        }
    }
}

} // namespace lw::win
