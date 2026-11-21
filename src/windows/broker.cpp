// SPDX-License-Identifier: GPL-3.0-or-later
#include "output_session.hpp"
#include "ffb_trace.hpp"

#include <cerrno>
#include <cwchar>
#include <stdexcept>

namespace {

using namespace lw::win;

UINT_PTR argument(const wchar_t* value) {
    wchar_t* end = nullptr;
    errno = 0;
    const auto parsed = std::wcstoull(value, &end, 10);
    if (errno != 0 || end == value || *end || *value == L'-' || parsed > UINTPTR_MAX) {
        throw std::runtime_error("Invalid inherited argument");
    }
    return static_cast<UINT_PTR>(parsed);
}

struct ReportRow {
    std::chrono::microseconds started;
    std::chrono::microseconds finished;
    bool success;
    lw::g29::OutputReport bytes;
};

std::ostream& operator<<(std::ostream& out, const ReportRow& row) {
    out << row.started.count() << ',' << row.finished.count() << ',' << row.success << ',';
    write_trace_bytes(out, row.bytes);
    return out;
}

class TracedOutput {
public:
    explicit TracedOutput(const wchar_t* log) : log_(log) {
        if (log_) {
            try {
                trace_ = std::make_unique<TraceBuffer<ReportRow>>();
            } catch (...) {
                OutputDebugStringW(L"LibreWheel: unable to start the USB output trace\n");
            }
        }
    }

    ~TracedOutput() {
        if (trace_) {
            try {
                trace_->write(log_, "time_us,completed_us,write_success,report_hex");
            } catch (...) {
                OutputDebugStringW(L"LibreWheel: unable to write the USB output trace\n");
            }
        }
    }

    bool send(HidOutput& output, const lw::g29::OutputReport& report) noexcept {
        if (!trace_) {
            return output.send(report);
        }
        const auto started = trace_time();
        const bool success = output.send(report);
        trace_->add({started, trace_time(), success, report});
        return success;
    }

private:
    const wchar_t* log_;
    std::unique_ptr<TraceBuffer<ReportRow>> trace_;
};

bool run(OutputSignals signals, OutputMailbox& mailbox, unsigned range, const std::wstring& path,
         const wchar_t* log) {
    TracedOutput trace(log);
    const auto owner = handle(CreateMutexW(nullptr, FALSE, L"Global\\LibreWheel.G29.Output"));
    if (!owner) {
        return false;
    }
    const auto ownership = WaitForSingleObject(owner.get(), 0);
    if (ownership != WAIT_OBJECT_0 && ownership != WAIT_ABANDONED) {
        return false;
    }
    struct Unlock {
        HANDLE mutex;
        ~Unlock() { ReleaseMutex(mutex); }
    } unlock{owner.get()};
    HidOutput output(path);
#ifdef LIBREWHEEL_FULL_FORCE
    constexpr auto strength = OutputStrength::full;
#else
    constexpr auto strength = OutputStrength::limited;
#endif
    struct Output {
        HidOutput& device;
        TracedOutput& trace;
        bool send(const lw::g29::OutputReport& report) noexcept { return trace.send(device, report); }
    } traced_output{output, trace};
    return drive_output(traced_output, signals, mailbox, range, strength);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 8 && argc != 9) {
        return 1;
    }
    try {
        const auto parent = handle(reinterpret_cast<HANDLE>(argument(argv[1])));
        const auto mapping = handle(reinterpret_cast<HANDLE>(argument(argv[2])));
        const auto changed = handle(reinterpret_cast<HANDLE>(argument(argv[3])));
        const auto stop = handle(reinterpret_cast<HANDLE>(argument(argv[4])));
        const auto ready = handle(reinterpret_cast<HANDLE>(argument(argv[5])));
        const auto range = argument(argv[6]);
        const MailboxView mailbox(static_cast<OutputMailbox*>(
            MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, sizeof(OutputMailbox))));
        if (!parent || !changed || !stop || !ready || !mailbox || range > 900) {
            return 1;
        }
        const OutputSignals signals{parent.get(), changed.get(), stop.get(), ready.get()};
        return run(signals, *mailbox, static_cast<unsigned>(range), argv[7], argc == 9 ? argv[8] : nullptr)
            ? 0 : 1;
    } catch (...) {
        return 1;
    }
}
