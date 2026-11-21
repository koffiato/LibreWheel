// SPDX-License-Identifier: GPL-3.0-or-later
#include "output_lease.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>

namespace lw::win {
namespace {

const char module_marker = 0;

std::filesystem::path broker_path() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_marker), &module)) {
        throw std::runtime_error("Cannot locate LibreWheel module");
    }
    std::wstring filename(32768, L'\0');
    const auto size = GetModuleFileNameW(module, filename.data(), static_cast<DWORD>(filename.size()));
    if (!size || size >= filename.size()) {
        throw std::runtime_error("Cannot resolve LibreWheel module path");
    }
    filename.resize(size);
    return std::filesystem::path(filename).parent_path() / L"LibreWheelOutput.exe";
}

std::wstring encode(HANDLE value) { return std::to_wstring(reinterpret_cast<UINT_PTR>(value)); }

struct DeleteAttributes {
    LPPROC_THREAD_ATTRIBUTE_LIST list;
    ~DeleteAttributes() { DeleteProcThreadAttributeList(list); }
};

} // namespace

OutputLease::OutputLease(const std::wstring& path, unsigned range, const std::filesystem::path& report_log) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    changed_ = handle(CreateEventW(&inherit, FALSE, FALSE, nullptr));
    stop_ = handle(CreateEventW(&inherit, TRUE, FALSE, nullptr));
    auto ready = handle(CreateEventW(&inherit, TRUE, FALSE, nullptr));
    auto mapping = handle(CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0,
                                             sizeof(OutputMailbox), nullptr));
    HANDLE parent_raw = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent_raw,
                         SYNCHRONIZE, TRUE, 0)) {
        throw std::runtime_error("Cannot duplicate host lifetime handle");
    }
    auto parent = handle(parent_raw);
    if (!changed_ || !stop_ || !ready || !mapping || path.find(L'"') != path.npos) {
        throw std::runtime_error("Cannot create output lease IPC");
    }
    mailbox_.reset(static_cast<OutputMailbox*>(MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0,
                                                            sizeof(OutputMailbox))));
    if (!mailbox_) {
        throw std::runtime_error("Cannot map output lease");
    }
    InterlockedExchange64(&mailbox_->latest, frame(0x80));

    const auto application = broker_path();
    std::wstring command = L"\"" + application.wstring() + L"\"";
    std::array inherited{parent.get(), mapping.get(), changed_.get(), stop_.get(), ready.get()};
    for (const HANDLE inherited_handle : inherited) {
        command += L" " + encode(inherited_handle);
    }
    command += L" " + std::to_wstring(range) + L" \"" + path + L"\"";
    if (!report_log.empty()) {
        command += L" \"" + report_log.wstring() + L"\"";
    }

    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (!bytes) {
        throw std::runtime_error("Cannot size output process attributes");
    }
    auto storage = std::make_unique<std::byte[]>(bytes);
    auto* list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.get());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) {
        throw std::runtime_error("Cannot initialize output process attributes");
    }
    const DeleteAttributes attributes{list};
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited.data(), sizeof(inherited), nullptr, nullptr)) {
        throw std::runtime_error("Cannot restrict the output process handles");
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = list;
    PROCESS_INFORMATION created{};
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                        application.parent_path().c_str(), &startup.StartupInfo, &created)) {
        throw std::runtime_error("Cannot launch the LibreWheel output process");
    }
    const auto primary = handle(created.hThread);
    process_ = handle(created.hProcess);

    const HANDLE events[]{process_.get(), ready.get()};
    if (WaitForMultipleObjects(2, events, FALSE, 1000) != WAIT_OBJECT_0 + 1) {
        SetEvent(stop_.get());
        throw std::runtime_error("The LibreWheel output process could not initialize the wheel");
    }
}

OutputLease::~OutputLease() {
    SetEvent(stop_.get());
    WaitForSingleObject(process_.get(), lease_timeout_ms + 100);
}

bool OutputLease::publish(std::uint8_t force) noexcept {
    if (WaitForSingleObject(process_.get(), 0) != WAIT_TIMEOUT) {
        return false;
    }
    const auto current = frame(force);
    InterlockedExchange64(&mailbox_->latest, current);
    const auto tick = static_cast<ULONGLONG>(current) >> 8;
    // Keep the older broker alive too. A new force byte always wakes it immediately.
    constexpr auto heartbeat_ms = lease_timeout_ms / 3;
    if (force == last_force_ && tick - last_signal_ < heartbeat_ms) {
        return true;
    }
    last_force_ = force;
    last_signal_ = tick;
    return SetEvent(changed_.get()) != FALSE;
}

} // namespace lw::win
