// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "hid.hpp"

namespace lw::win {

inline constexpr DWORD lease_timeout_ms = 150;
inline constexpr wchar_t output_mutex_name[] = L"Global\\LibreWheel.G29.Output";
inline constexpr wchar_t user_stop_event_name[] = L"Local\\LibreWheel.Stop";

enum class BrokerExit : DWORD {
    stopped = 0,
    invalid_args = 1,
    watchdog_timeout = 2,
    user_stop = 3,
    device_error = 4,
};

enum class PublishStatus {
    ok,
    watchdog_timeout,
    user_stop,
    external_kill,
    device_error,
};

struct alignas(8) OutputMailbox {
    volatile LONG64 latest; // GetTickCount64 in the high bits, classic force byte in the low 8.
};

inline LONG64 frame(std::uint8_t force) noexcept {
    return static_cast<LONG64>((GetTickCount64() << 8) | force);
}

struct Unmap { void operator()(void* pointer) const noexcept { UnmapViewOfFile(pointer); } };
using MailboxView = std::unique_ptr<OutputMailbox, Unmap>;

class OutputLease {
public:
    OutputLease(const std::wstring& path, unsigned range, const std::filesystem::path& report_log,
                const std::filesystem::path& broker_override = {});
    ~OutputLease();
    OutputLease(const OutputLease&) = delete;
    OutputLease& operator=(const OutputLease&) = delete;

    HANDLE process_handle() const noexcept { return process_.get(); }
    PublishStatus check_exit() const noexcept;
    PublishStatus publish(std::uint8_t force) noexcept;

private:
    Handle changed_;
    Handle stop_;
    MailboxView mailbox_;
    Handle process_;
    ULONGLONG last_signal_ = 0;
    std::uint8_t last_force_ = 0x80;
};

} // namespace lw::win
