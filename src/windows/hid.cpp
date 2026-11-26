// SPDX-License-Identifier: GPL-3.0-or-later
#include "hid.hpp"

extern "C" {
#include <hidsdi.h>
}
#include <hidusage.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>

namespace lw::win {
namespace {

[[noreturn]] void win_error(const char* operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

Handle open(const std::wstring& path, DWORD access) {
    auto file = handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
    if (!file) {
        win_error("Open HID collection");
    }
    return file;
}

PHIDP_PREPARSED_DATA preparsed(HANDLE file) {
    PHIDP_PREPARSED_DATA descriptor = nullptr;
    if (!HidD_GetPreparsedData(file, &descriptor)) {
        win_error("Read HID descriptor");
    }
    return descriptor;
}

using Descriptor = std::unique_ptr<_HIDP_PREPARSED_DATA, decltype([](PHIDP_PREPARSED_DATA data) {
    HidD_FreePreparsedData(data);
})>;

bool write_report(HANDLE file, HANDLE event, std::span<const std::uint8_t> report) noexcept {
    OVERLAPPED operation{};
    ResetEvent(event);
    operation.hEvent = event;
    if (!WriteFile(file, report.data(), static_cast<DWORD>(report.size()), nullptr, &operation) &&
        GetLastError() != ERROR_IO_PENDING) {
        return false;
    }
    DWORD transferred = 0;
    if (!GetOverlappedResultEx(file, &operation, &transferred, 40, FALSE)) {
        CancelIoEx(file, &operation);
        GetOverlappedResult(file, &operation, &transferred, TRUE);
        return false;
    }
    return transferred == report.size();
}

bool classic_output(PHIDP_PREPARSED_DATA descriptor, const HIDP_CAPS& caps) {
    if (caps.OutputReportByteLength != g29::OutputReport{}.size()) {
        return false;
    }
    std::vector<HIDP_VALUE_CAPS> values(caps.NumberOutputValueCaps);
    USHORT count = caps.NumberOutputValueCaps;
    if (HidP_GetValueCaps(HidP_Output, values.data(), &count, descriptor) != HIDP_STATUS_SUCCESS) {
        return false;
    }
    for (USHORT index = 0; index < count; ++index) {
        const auto& value = values[index];
        if (value.ReportID == 0 && value.UsagePage == 0xff00 && !value.IsRange &&
            value.NotRange.Usage == 2 && value.BitSize == 8 && value.ReportCount == 16) {
            return true;
        }
    }
    return false;
}

std::optional<DeviceInfo> identify(const std::wstring& path, HANDLE file, PHIDP_PREPARSED_DATA descriptor) {
    HIDD_ATTRIBUTES attributes{};
    attributes.Size = sizeof(attributes);
    HIDP_CAPS caps{};
    if (!HidD_GetAttributes(file, &attributes) ||
        attributes.VendorID != g29::vendor_id || attributes.ProductID != g29::product_id ||
        HidP_GetCaps(descriptor, &caps) != HIDP_STATUS_SUCCESS ||
        caps.UsagePage != HID_USAGE_PAGE_GENERIC || caps.Usage != HID_USAGE_GENERIC_JOYSTICK ||
        caps.InputReportByteLength == 0) {
        return std::nullopt;
    }
    return DeviceInfo{path, attributes.VersionNumber, caps.InputReportByteLength,
                      caps.OutputReportByteLength, classic_output(descriptor, caps)};
}

} // namespace

GUID hid_interface_guid() noexcept {
    GUID guid{};
    HidD_GetHidGuid(&guid);
    return guid;
}

std::filesystem::path settings_path() {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!length) {
        win_error("Resolve LOCALAPPDATA");
    }
    std::wstring directory(length, L'\0');
    const auto copied = GetEnvironmentVariableW(L"LOCALAPPDATA", directory.data(), length);
    if (!copied || copied >= length) {
        win_error("Read LOCALAPPDATA");
    }
    directory.resize(copied);
    return std::filesystem::path(directory) / L"LibreWheel" / L"settings.ini";
}

namespace {

constexpr std::wstring_view compatibility_interface = L"hid#vid_046d&pid_c294";
constexpr std::wstring_view native_interface = L"hid#vid_046d&pid_c24f";

bool matches_interface(std::wstring_view path, std::wstring_view id) noexcept {
    if (!path.starts_with(L"\\\\?\\") && !path.starts_with(L"\\??\\")) {
        return false;
    }
    path.remove_prefix(4);
    const auto length = static_cast<int>(id.size());
    return path.size() > id.size() && (path[id.size()] == L'&' || path[id.size()] == L'#') &&
        CompareStringOrdinal(path.data(), length, id.data(), length, TRUE) == CSTR_EQUAL;
}

std::vector<std::wstring> paths_for(std::wstring_view id) {
    const auto guid = hid_interface_guid();
    const auto devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        win_error("Enumerate HID collections");
    }
    const std::unique_ptr<void, decltype([](HDEVINFO info) { SetupDiDestroyDeviceInfoList(info); })>
        owned_devices(devices);

    std::vector<std::wstring> result;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &interface_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            win_error("Read HID interface list");
        }
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }
        auto buffer = std::make_unique<std::byte[]>(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.get());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, size, nullptr, nullptr)) {
            continue;
        }
        const std::wstring_view path(detail->DevicePath);
        if (matches_interface(path, id)) {
            result.emplace_back(path);
        }
    }
    return result;
}

unsigned compatibility_report_size(HANDLE file) {
    HIDD_ATTRIBUTES attributes{};
    attributes.Size = sizeof(attributes);
    if (!HidD_GetAttributes(file, &attributes) ||
        !g29::native_switch_candidate(attributes.VendorID, attributes.ProductID, attributes.VersionNumber)) {
        return 0;
    }
    const Descriptor descriptor(preparsed(file));
    HIDP_CAPS caps{};
    if (HidP_GetCaps(descriptor.get(), &caps) != HIDP_STATUS_SUCCESS ||
        caps.UsagePage != HID_USAGE_PAGE_GENERIC || caps.Usage != HID_USAGE_GENERIC_GAMEPAD ||
        caps.OutputReportByteLength < g29::switch_to_native_mode.size()) {
        return 0;
    }
    std::vector<HIDP_VALUE_CAPS> values(caps.NumberOutputValueCaps);
    USHORT count = caps.NumberOutputValueCaps;
    if (HidP_GetValueCaps(HidP_Output, values.data(), &count, descriptor.get()) != HIDP_STATUS_SUCCESS) {
        return 0;
    }
    for (USHORT i = 0; i < count; ++i) {
        const auto& value = values[i];
        if (value.ReportID == 0 && value.UsagePage == 0xff00 && value.BitSize == 8 &&
            value.ReportCount >= g29::switch_to_native_mode.size() - 1) {
            return caps.OutputReportByteLength;
        }
    }
    return 0;
}

std::vector<std::wstring> compatibility_paths() {
    std::vector<std::wstring> result;
    for (auto& path : paths_for(compatibility_interface)) {
        try {
            const auto file = open(path, 0);
            if (compatibility_report_size(file.get())) {
                result.push_back(std::move(path));
            }
        } catch (const std::system_error&) {
            // The device may disappear between enumeration and opening.
        }
    }
    return result;
}

} // namespace

std::vector<DeviceInfo> discover() {
    std::vector<DeviceInfo> result;
    for (const auto& path : paths_for(L"hid#vid_046d&pid_c24f")) {
        try {
            auto file = open(path, 0);
            const Descriptor descriptor(preparsed(file.get()));
            if (auto info = identify(path, file.get(), descriptor.get())) {
                result.push_back(std::move(*info));
            }
        } catch (const std::system_error&) {
            // The device may disappear between enumeration and opening.
        }
    }
    return result;
}

bool compatibility_connected() {
    return !compatibility_paths().empty();
}

bool is_compatibility_interface(std::wstring_view path) noexcept {
    return matches_interface(path, compatibility_interface);
}

bool is_native_interface(std::wstring_view path) noexcept {
    return matches_interface(path, native_interface);
}

std::optional<DeviceInfo> initialize_native(std::stop_token stop) {
    using namespace std::chrono_literals;

    auto native = discover();
    if (native.size() == 1) {
        return std::move(native.front());
    }
    if (native.size() > 1) {
        throw std::runtime_error("Connect one G29 before initializing it");
    }

    const auto compatibility = compatibility_paths();
    if (compatibility.empty()) {
        return std::nullopt;
    }
    if (compatibility.size() > 1) {
        throw std::runtime_error("Connect one G29 before initializing it");
    }
    if (stop.stop_requested()) {
        return std::nullopt;
    }

    const auto file = open(compatibility.front(), GENERIC_WRITE);
    const auto report_size = compatibility_report_size(file.get());
    if (!report_size) {
        throw std::runtime_error("G29 compatibility collection changed during initialization");
    }
    const auto event = handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
        win_error("Create mode-switch event");
    }
    std::vector<std::uint8_t> report(report_size);
    std::ranges::copy(g29::reset_mode_on_usb_reset, report.begin());
    if (!write_report(file.get(), event.get(), report)) {
        throw std::runtime_error("G29 did not accept reset-on-USB-reset mode");
    }
    std::ranges::copy(g29::switch_to_native_mode, report.begin());
    const bool acknowledged = write_report(file.get(), event.get(), report);

    // The second write can lose its completion when the wheel detaches.
    // The native collection is the success criterion.
    std::mutex wait_mutex;
    std::condition_variable_any wait_cv;
    std::unique_lock wait_lock(wait_mutex);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        native = discover();
        if (native.size() == 1) {
            return std::move(native.front());
        }
        if (native.size() > 1) {
            throw std::runtime_error("More than one G29 appeared after initialization");
        }
        if (wait_cv.wait_for(wait_lock, stop, 100ms, [] { return false; }) || stop.stop_requested()) {
            return std::nullopt;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(acknowledged ? "G29 did not re-enumerate in native mode"
                                          : "G29 mode-switch write failed and no native collection appeared");
}

void HidInput::FreePreparsed::operator()(void* descriptor) const noexcept {
    HidD_FreePreparsedData(static_cast<PHIDP_PREPARSED_DATA>(descriptor));
}

HidInput::HidInput(const std::wstring& path)
    : file_(open(path, GENERIC_READ)), event_(handle(CreateEventW(nullptr, TRUE, FALSE, nullptr))),
      descriptor_(preparsed(file_.get())) {
    if (!event_) {
        win_error("Create input event");
    }
    auto info = identify(path, file_.get(), static_cast<PHIDP_PREPARSED_DATA>(descriptor_.get()));
    if (!info) {
        throw std::runtime_error("Expected the native G29 joystick collection");
    }
    info_ = std::move(*info);
    buffer_.resize(info_.input_bytes);
    latest_.resize(info_.input_bytes);
}

HidInput::~HidInput() {
    if (pending_) {
        CancelIoEx(file_.get(), &operation_);
        DWORD transferred = 0;
        GetOverlappedResult(file_.get(), &operation_, &transferred, TRUE);
    }
}

void HidInput::begin() {
    if (pending_) {
        return;
    }
    ResetEvent(event_.get());
    operation_ = {};
    operation_.hEvent = event_.get();
    if (ReadFile(file_.get(), buffer_.data(), static_cast<DWORD>(buffer_.size()), nullptr, &operation_)) {
        SetEvent(event_.get());
    } else if (GetLastError() != ERROR_IO_PENDING) {
        win_error("Read G29 input");
    }
    pending_ = true;
}

bool HidInput::take_report() {
    if (!pending_) {
        return false;
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(file_.get(), &operation_, &transferred, FALSE)) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_INCOMPLETE) {
            return false;
        }
        pending_ = false;
        win_error("Complete G29 input read", error);
    }
    pending_ = false;
    if (transferred != buffer_.size() || buffer_.front() != 0) {
        win_error("Unexpected G29 input report", ERROR_INVALID_DATA);
    }
    buffer_.swap(latest_);
    return true;
}

unsigned HidInput::usage(unsigned short id) {
    const auto descriptor = static_cast<PHIDP_PREPARSED_DATA>(descriptor_.get());
    ULONG value = 0;
    if (HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, id, &value, descriptor,
                          latest_.data(), static_cast<ULONG>(latest_.size()))
        != HIDP_STATUS_SUCCESS) {
        win_error("Unexpected G29 input descriptor", ERROR_INVALID_DATA);
    }
    return value;
}

std::optional<unsigned> HidInput::take_steering() {
    if (!take_report()) {
        return std::nullopt;
    }
    return usage(HID_USAGE_GENERIC_X);
}

std::optional<unsigned> HidInput::drain_steering(unsigned max_reports) {
    bool received = false;
    for (unsigned count = 0; count < max_reports; ++count) {
        if (!take_report()) {
            break;
        }
        received = true;
        begin();
    }
    return received ? std::optional<unsigned>(usage(HID_USAGE_GENERIC_X)) : std::nullopt;
}

std::optional<Input> HidInput::take() {
    if (!take_report()) {
        return std::nullopt;
    }
    Input input{usage(HID_USAGE_GENERIC_X), usage(HID_USAGE_GENERIC_Z), usage(HID_USAGE_GENERIC_RZ),
                usage(HID_USAGE_GENERIC_Y), usage(HID_USAGE_GENERIC_HATSWITCH), 0};
    std::array<USAGE, 25> buttons{};
    ULONG count = static_cast<ULONG>(buttons.size());
    const auto descriptor = static_cast<PHIDP_PREPARSED_DATA>(descriptor_.get());
    if (HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, buttons.data(), &count,
                       descriptor, latest_.data(), static_cast<ULONG>(latest_.size())) != HIDP_STATUS_SUCCESS) {
        win_error("Unexpected G29 button descriptor", ERROR_INVALID_DATA);
    }
    for (ULONG index = 0; index < count; ++index) {
        if (buttons[index] >= 1 && buttons[index] <= 25) {
            input.buttons |= std::uint32_t{1} << (buttons[index] - 1);
        }
    }
    return input;
}

HidOutput::HidOutput(const std::wstring& path)
    : file_(open(path, GENERIC_WRITE)), event_(handle(CreateEventW(nullptr, TRUE, FALSE, nullptr))) {
    if (!event_) {
        win_error("Create output event");
    }
    const Descriptor descriptor(preparsed(file_.get()));
    const auto info = identify(path, file_.get(), descriptor.get());
    if (!info || !info->classic_output) {
        throw std::runtime_error("Expected the unnumbered G29 classic output report");
    }
}

bool HidOutput::send(const g29::OutputReport& report) noexcept {
    return write_report(file_.get(), event_.get(), report);
}

} // namespace lw::win
