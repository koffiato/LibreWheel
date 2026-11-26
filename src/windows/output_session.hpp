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
    HANDLE user_stop = nullptr;
};

enum class OutputStrength { limited, full };

template<class Output>
BrokerExit drive_output(Output& output, OutputSignals signals, OutputMailbox& mailbox, unsigned range,
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

    HANDLE events[4]{signals.stop, signals.parent, nullptr, nullptr};
    DWORD stop_count = 2;
    DWORD event_count = 3;
    if (signals.user_stop) {
        events[2] = signals.user_stop;
        events[3] = signals.changed;
        stop_count = 3;
        event_count = 4;
    } else {
        events[2] = signals.changed;
    }

    const auto precheck = WaitForMultipleObjects(stop_count, events, FALSE, 0);
    if (precheck == WAIT_OBJECT_0 || precheck == WAIT_OBJECT_0 + 1) {
        return BrokerExit::stopped;
    }
    if (signals.user_stop && precheck == WAIT_OBJECT_0 + 2) {
        return BrokerExit::user_stop;
    }
    if (precheck != WAIT_TIMEOUT) {
        return BrokerExit::device_error;
    }
    if (!output.send(g29::torque(idle)) || !output.send(g29::disable_centering())) {
        return BrokerExit::device_error;
    }
    if (range) {
        const auto report = g29::operating_range(range);
        if (!report || !output.send(*report)) {
            return BrokerExit::device_error;
        }
    }
    if (!SetEvent(signals.ready)) {
        return BrokerExit::device_error;
    }

    std::uint8_t sent = idle;
    auto latest_tick = GetTickCount64();
    while (true) {
        DWORD remaining = INFINITE;
        if (sent != idle) {
            const auto age = GetTickCount64() - latest_tick;
            remaining = age < lease_timeout_ms ? lease_timeout_ms - static_cast<DWORD>(age) : 0;
        }
        const auto wake = WaitForMultipleObjects(event_count, events, FALSE, remaining);
        if (wake == WAIT_FAILED) {
            return BrokerExit::device_error;
        }
        if (wake == WAIT_OBJECT_0 || wake == WAIT_OBJECT_0 + 1) {
            return BrokerExit::stopped;
        }
        if (signals.user_stop && wake == WAIT_OBJECT_0 + 2) {
            return BrokerExit::user_stop;
        }
        // A steady force may refresh the shared timestamp without signaling changed.
        const auto latest = static_cast<ULONGLONG>(InterlockedCompareExchange64(&mailbox.latest, 0, 0));
        latest_tick = latest >> 8;
        const auto requested = static_cast<std::uint8_t>(latest & 0xff);
        const bool stale = GetTickCount64() - latest_tick >= lease_timeout_ms;
        if (stale && (sent != idle || requested != idle)) {
            return BrokerExit::watchdog_timeout;
        }
        // Limit bench builds to about 10% of the classic force-byte excursion.
        const auto force = strength == OutputStrength::limited
            ? std::clamp(requested, std::uint8_t{0x74}, std::uint8_t{0x8c})
            : requested;
        if (force != sent) {
            if (!output.send(g29::torque(force))) {
                return BrokerExit::device_error;
            }
            sent = force;
        }
    }
}

} // namespace lw::win
