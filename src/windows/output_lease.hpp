// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "hid.hpp"

namespace lw::win {

inline constexpr DWORD lease_timeout_ms = 150;

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
    OutputLease(const std::wstring& path, unsigned range);
    ~OutputLease();
    OutputLease(const OutputLease&) = delete;
    OutputLease& operator=(const OutputLease&) = delete;

    bool publish(std::uint8_t force) noexcept;

private:
    Handle changed_;
    Handle stop_;
    MailboxView mailbox_;
    Handle process_;
};

} // namespace lw::win
