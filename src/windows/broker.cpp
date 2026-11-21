// SPDX-License-Identifier: GPL-3.0-or-later
#include "output_session.hpp"

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

bool run(OutputSignals signals, OutputMailbox& mailbox, unsigned range, const std::wstring& path) {
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
    return drive_output(output, signals, mailbox, range, strength);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 8) {
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
        return run(signals, *mailbox, static_cast<unsigned>(range), argv[7]) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}
