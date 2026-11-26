// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "protocol.hpp"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace lw::win {

using Handle = std::unique_ptr<void, decltype([](HANDLE h) { CloseHandle(h); })>;
inline Handle handle(HANDLE h) { return Handle(h == INVALID_HANDLE_VALUE ? nullptr : h); }

struct DeviceInfo {
    std::wstring path;
    unsigned revision;
    unsigned input_bytes;
    unsigned output_bytes;
    bool classic_output;
};

struct Input {
    unsigned steering;
    unsigned accelerator;
    unsigned brake;
    unsigned clutch;
    unsigned hat;
    std::uint32_t buttons;
};

std::vector<DeviceInfo> discover();
bool compatibility_connected();
bool is_compatibility_interface(std::wstring_view path) noexcept;
bool is_native_interface(std::wstring_view path) noexcept;
std::optional<DeviceInfo> initialize_native(std::stop_token stop = {});
GUID hid_interface_guid() noexcept;
std::filesystem::path settings_path();

class HidInput {
public:
    explicit HidInput(const std::wstring& path);
    ~HidInput();
    HidInput(const HidInput&) = delete;
    HidInput& operator=(const HidInput&) = delete;

    const DeviceInfo& info() const noexcept { return info_; }
    HANDLE ready() const noexcept { return event_.get(); }
    void begin();
    std::optional<Input> take();
    std::optional<unsigned> take_steering();
    std::optional<unsigned> drain_steering(unsigned max_reports = 64);

private:
    bool take_report();
    unsigned usage(unsigned short id);
    struct FreePreparsed { void operator()(void*) const noexcept; };
    Handle file_;
    Handle event_;
    std::unique_ptr<void, FreePreparsed> descriptor_;
    DeviceInfo info_;
    std::vector<char> buffer_;
    std::vector<char> latest_;
    OVERLAPPED operation_{};
    bool pending_ = false;
};

// This transport is used by the output owner during physical validation.
class HidOutput {
public:
    explicit HidOutput(const std::wstring& path);
    bool send(const g29::OutputReport& report) noexcept;

private:
    Handle file_;
    Handle event_;
};

} // namespace lw::win
