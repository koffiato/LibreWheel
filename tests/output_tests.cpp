// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows/output_session.hpp"

#include <array>
#include <iostream>
#include <thread>

namespace {

using namespace lw::win;
using namespace lw::g29;

enum class End { host_exit, stale, explicit_stop, send_error };

struct Sink {
    HANDLE applied;
    bool fail;
    std::array<OutputReport, 8> reports{};
    std::size_t used = 0;

    bool send(const OutputReport& report) noexcept {
        reports.at(used++) = report;
        if (report[1] == 0x11) {
            SetEvent(applied);
            return !fail;
        }
        return true;
    }
};

bool verify(End reason, OutputStrength strength) {
    const auto parent = handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const auto changed = handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    const auto stop = handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const auto ready = handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const auto applied = handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!(parent && changed && stop && ready && applied)) {
        return false;
    }
    const OutputSignals signals{parent.get(), changed.get(), stop.get(), ready.get()};
    OutputMailbox mailbox{};
    Sink sink{applied.get(), reason == End::send_error};
    bool result = false;
    std::thread worker([&] { result = drive_output(sink, signals, mailbox, 900, strength); });
    const bool started = WaitForSingleObject(ready.get(), 1000) == WAIT_OBJECT_0;
    InterlockedExchange64(&mailbox.latest, frame(0xff));
    SetEvent(changed.get());
    const bool wrote = WaitForSingleObject(applied.get(), 1000) == WAIT_OBJECT_0;
    if (!started || !wrote || reason == End::explicit_stop) {
        SetEvent(stop.get());
    } else if (reason == End::host_exit) {
        SetEvent(parent.get());
    }
    worker.join();
    const std::uint8_t expected = strength == OutputStrength::full ? 0xff : 0x8c;
    return started && wrote && result == (reason != End::send_error) && sink.used == 5 &&
        sink.reports[0] == torque(0x80) && sink.reports[1] == disable_centering() &&
        sink.reports[2] == operating_range(900) && sink.reports[3] == torque(expected) &&
        sink.reports[4] == torque(0x80);
}

} // namespace

int main() {
    for (auto end : {End::host_exit, End::stale, End::explicit_stop, End::send_error}) {
        if (!verify(end, OutputStrength::limited) || !verify(end, OutputStrength::full)) {
            std::cerr << "Output shutdown fixture failed: " << static_cast<int>(end) << '\n';
            return 1;
        }
    }
    return 0;
}
