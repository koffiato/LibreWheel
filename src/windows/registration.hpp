// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lw::win {

inline constexpr wchar_t driver_class_text[] = L"{00EB836B-81CB-4D34-B0DF-16202A83BD4A}";
inline constexpr wchar_t oem_key_path[] =
    L"System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_046D&PID_C24F";
inline constexpr wchar_t force_feedback_key_path[] =
    L"System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_046D&PID_C24F\\OEMForceFeedback";
inline constexpr wchar_t axis0_key_path[] =
    L"System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_046D&PID_C24F\\Axes\\0";
inline constexpr wchar_t com_class_key_path[] =
    L"Software\\Classes\\CLSID\\{00EB836B-81CB-4D34-B0DF-16202A83BD4A}";
inline constexpr wchar_t owner_value_name[] = L"LibreWheelOwner";

struct ConflictInfo {
    bool machine_hive = false;
    std::wstring clsid;
    std::wstring server_path;
    bool is_logitech = false;
};

bool is_process_elevated() noexcept;
bool validate_driver_dll(const std::filesystem::path& path, std::uint16_t expected_machine) noexcept;
std::vector<ConflictInfo> detect_oem_conflicts();
bool has_oem_conflict(bool machine_hive) noexcept;
bool is_librewheel_registered() noexcept;

// Exports a Windows Registry Editor Version 5.00 UTF-16 LE .reg backup of root\subkey.
std::filesystem::path export_reg_key(HKEY root, const wchar_t* root_name, const wchar_t* subkey,
                                     const std::filesystem::path& output_file);

// Formats an in-memory registry key entry for .reg export testing and serialization.
struct RegExportValue {
    std::wstring name;
    DWORD type = REG_SZ;
    std::vector<std::uint8_t> data;
};
struct RegExportNode {
    std::wstring full_path;
    std::vector<RegExportValue> values;
    std::vector<RegExportNode> children;
};
std::wstring format_reg_document(const RegExportNode& root);

// Exports a timestamped backup into backup_dir and deletes a non-LibreWheel OEMForceFeedback key.
// Returns the backup file path if a key was removed, or std::nullopt if no conflict existed.
std::optional<std::filesystem::path> backup_and_remove_conflict(bool machine_hive,
                                                                const std::filesystem::path& backup_dir);

void install_or_switch_registration(const std::filesystem::path& dll64,
                                    const std::filesystem::path& dll32,
                                    bool validate_only = false,
                                    bool allow_existing_oem = false);

void remove_registration(const std::filesystem::path& expected_dll64 = {},
                         const std::filesystem::path& expected_dll32 = {});

} // namespace lw::win
