// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows/ffb_trace.hpp"
#include "windows/output_session.hpp"

#include <array>
#include <iostream>
#include <thread>

namespace {

using namespace lw::win;
using namespace lw::g29;
using namespace std::chrono_literals;

enum class End { host_exit, stale, explicit_stop, user_stop, send_error, steady_force, idle_wait };

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
    const auto user_stop = handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const auto applied = handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!(parent && changed && stop && ready && user_stop && applied)) {
        return false;
    }
    const OutputSignals signals{parent.get(), changed.get(), stop.get(), ready.get(), user_stop.get()};
    OutputMailbox mailbox{};
    InterlockedExchange64(&mailbox.latest, frame(0x80));
    Sink sink{applied.get(), reason == End::send_error};
    BrokerExit result = BrokerExit::device_error;
    std::thread worker([&] { result = drive_output(sink, signals, mailbox, 900, strength); });
    const bool started = WaitForSingleObject(ready.get(), 1000) == WAIT_OBJECT_0;
    if (reason == End::idle_wait) {
        // An idle-neutral broker must stay alive past the watchdog timeout without exiting.
        std::this_thread::sleep_for(std::chrono::milliseconds(lease_timeout_ms + 60));
        InterlockedExchange64(&mailbox.latest, frame(0xff));
        SetEvent(changed.get());
        const bool wrote = WaitForSingleObject(applied.get(), 1000) == WAIT_OBJECT_0;
        SetEvent(stop.get());
        worker.join();
        return started && wrote && result == BrokerExit::stopped && sink.used == 5u &&
            sink.reports[3] == torque(0xff) && sink.reports[4] == torque(0x80);
    }
    InterlockedExchange64(&mailbox.latest, frame(0xff));
    SetEvent(changed.get());
    const bool wrote = WaitForSingleObject(applied.get(), 1000) == WAIT_OBJECT_0;
    bool second_force_written = false;
    if (!started || !wrote || reason == End::explicit_stop) {
        SetEvent(stop.get());
    } else if (reason == End::host_exit) {
        SetEvent(parent.get());
    } else if (reason == End::user_stop) {
        SetEvent(user_stop.get());
    } else if (reason == End::steady_force) {
        // Refresh the mailbox through an entire watchdog interval without waking the broker.
        for (unsigned i = 0; i < 8; ++i) {
            std::this_thread::sleep_for(25ms);
            InterlockedExchange64(&mailbox.latest, frame(0xff));
        }
        ResetEvent(applied.get());
        InterlockedExchange64(&mailbox.latest, frame(0x40));
        SetEvent(changed.get());
        second_force_written = WaitForSingleObject(applied.get(), 500) == WAIT_OBJECT_0;
        SetEvent(stop.get());
    }
    worker.join();
    BrokerExit expected_exit = BrokerExit::stopped;
    if (reason == End::send_error) expected_exit = BrokerExit::device_error;
    else if (reason == End::stale) expected_exit = BrokerExit::watchdog_timeout;
    else if (reason == End::user_stop) expected_exit = BrokerExit::user_stop;

    const std::uint8_t expected = strength == OutputStrength::full ? 0xff : 0x8c;
    const bool steady = reason == End::steady_force;
    const std::uint8_t second = strength == OutputStrength::full ? 0x40 : 0x74;
    return started && wrote && result == expected_exit && sink.used == (steady ? 6u : 5u) &&
        sink.reports[0] == torque(0x80) && sink.reports[1] == disable_centering() &&
        sink.reports[2] == operating_range(900) && sink.reports[3] == torque(expected) &&
        (!steady || (second_force_written && sink.reports[4] == torque(second))) &&
        sink.reports[sink.used - 1] == torque(0x80);
}

bool trace_ring() {
    TraceBuffer<std::size_t> trace;
    constexpr std::size_t overwritten = 19;
    for (std::size_t row = 0; row < trace_capacity + overwritten; ++row) {
        trace.add(row);
    }
    const auto path = std::filesystem::path(L"trace-ring-test-" + std::to_wstring(GetCurrentProcessId()) + L".csv");
    trace.write(path, "row");
    std::ifstream input(path);
    std::string line;
    bool valid = std::getline(input, line) && line == "# dropped_rows=19" &&
                 std::getline(input, line) && line == "row";
    for (std::size_t row = overwritten; valid && row < trace_capacity + overwritten; ++row) {
        std::size_t saved = 0;
        valid = (input >> saved) && saved == row;
    }
    valid = valid && !(input >> line) && input.eof();
    input.close();
    std::filesystem::remove(path);
    return valid;
}

bool verify_ipc(const std::filesystem::path& broker_exe) {
    const auto test_stop = handle(CreateEventW(nullptr, TRUE, FALSE, L"Local\\LibreWheel.Test.Stop"));
    if (!test_stop) {
        return false;
    }
    struct ResetStop {
        HANDLE event;
        ~ResetStop() { ResetEvent(event); }
    } reset_stop{test_stop.get()};
    ResetEvent(test_stop.get());

    // 1. Clean lifecycle + USB report log verification across real process IPC.
    const auto usb_log = std::filesystem::current_path() /
        (L"ipc-usb-test-" + std::to_wstring(GetCurrentProcessId()) + L".csv");
    struct Cleanup {
        std::filesystem::path file;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(file, error);
        }
    } cleanup{usb_log};
    {
        OutputLease lease(L"test-fixture", 900, usb_log, broker_exe);
        if (lease.publish(0xff) != PublishStatus::ok) {
            return false;
        }
        std::this_thread::sleep_for(25ms);
        if (lease.publish(0x80) != PublishStatus::ok) {
            return false;
        }
        std::this_thread::sleep_for(25ms);
    }
    {
        std::ifstream input(usb_log);
        std::string line;
        std::vector<std::string> hexes;
        while (std::getline(input, line)) {
            if (line.empty() || line.starts_with('#') || line.starts_with("time_us,")) {
                continue;
            }
            const auto comma = line.rfind(',');
            if (comma != std::string::npos) {
                hexes.push_back(line.substr(comma + 1));
            }
        }
        if (hexes.size() < 6 || hexes[0].substr(0, 4) != "0013" || hexes[1].substr(0, 4) != "00f5" ||
            hexes[2].substr(0, 10) != "00f8818403" || hexes[3].substr(0, 10) != "001108ff80" ||
            hexes.back().substr(0, 4) != "0013") {
            std::cerr << "Unexpected broker IPC report trace (" << hexes.size() << " rows)\n";
            return false;
        }
    }

    // 2. Real out-of-process watchdog timeout while torque is active.
    {
        OutputLease lease(L"test-fixture", 0, {}, broker_exe);
        if (lease.publish(0x40) != PublishStatus::ok) {
            return false;
        }
        if (WaitForSingleObject(lease.process_handle(), lease_timeout_ms + 500) != WAIT_OBJECT_0 ||
            lease.check_exit() != PublishStatus::watchdog_timeout) {
            std::cerr << "Broker IPC watchdog timeout did not report watchdog_timeout\n";
            return false;
        }
    }

    // 3. Real out-of-process user/tray stop event.
    {
        OutputLease lease(L"test-fixture", 0, {}, broker_exe);
        SetEvent(test_stop.get());
        if (WaitForSingleObject(lease.process_handle(), 1000) != WAIT_OBJECT_0 ||
            lease.check_exit() != PublishStatus::user_stop) {
            std::cerr << "Broker IPC user stop event did not report user_stop\n";
            return false;
        }
        ResetEvent(test_stop.get());
    }

    // 4. Real out-of-process external kill (Task Manager TerminateProcess).
    {
        OutputLease lease(L"test-fixture", 0, {}, broker_exe);
        if (!TerminateProcess(lease.process_handle(), 1) ||
            WaitForSingleObject(lease.process_handle(), 1000) != WAIT_OBJECT_0 ||
            lease.check_exit() != PublishStatus::external_kill) {
            std::cerr << "Broker IPC external termination did not report external_kill\n";
            return false;
        }
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    for (auto end : {End::host_exit, End::stale, End::explicit_stop, End::user_stop,
                     End::send_error, End::steady_force, End::idle_wait}) {
        // The steady-force fixture exercises both ends of the bench clamp.
        const auto strength = end == End::steady_force ? OutputStrength::limited : OutputStrength::full;
        if (!verify(end, strength)) {
            std::cerr << "Output shutdown fixture failed: " << static_cast<int>(end) << '\n';
            return 1;
        }
    }
    if (!trace_ring()) {
        std::cerr << "Trace ring did not preserve the retained rows and drop count\n";
        return 1;
    }
    if (argc == 2 && !verify_ipc(argv[1])) {
        std::cerr << "Out-of-process broker IPC integration test failed\n";
        return 1;
    }
    return 0;
}
