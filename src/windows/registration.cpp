// SPDX-License-Identifier: GPL-3.0-or-later
#include "registration.hpp"

#include "driver.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lw::win {
namespace {

struct RegClose {
    void operator()(HKEY key) const noexcept { RegCloseKey(key); }
};
using RegKey = std::unique_ptr<std::remove_pointer_t<HKEY>, RegClose>;

RegKey open_key(HKEY root, const wchar_t* subkey, REGSAM access) noexcept {
    HKEY raw = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, access, &raw) != ERROR_SUCCESS) {
        return {};
    }
    return RegKey(raw);
}

RegKey create_key(HKEY root, const wchar_t* subkey, REGSAM access) {
    HKEY raw = nullptr;
    const auto status = RegCreateKeyExW(root, subkey, 0, nullptr, 0, access, nullptr, &raw, nullptr);
    if (status != ERROR_SUCCESS) {
        throw std::runtime_error("Unable to create registry key (Win32 error " + std::to_string(status) + ")");
    }
    return RegKey(raw);
}

bool key_exists(HKEY root, const wchar_t* subkey, REGSAM view = KEY_WOW64_64KEY) noexcept {
    return static_cast<bool>(open_key(root, subkey, KEY_READ | view));
}

std::optional<std::wstring> query_string(HKEY root, const wchar_t* subkey, const wchar_t* value_name,
                                         REGSAM view = KEY_WOW64_64KEY) {
    const auto key = open_key(root, subkey, KEY_READ | view);
    if (!key) {
        return std::nullopt;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key.get(), value_name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key.get(), value_name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(value.data()), &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

std::optional<std::vector<std::uint8_t>> query_binary(HKEY root, const wchar_t* subkey, const wchar_t* value_name,
                                                      REGSAM view = KEY_WOW64_64KEY) {
    const auto key = open_key(root, subkey, KEY_READ | view);
    if (!key) {
        return std::nullopt;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key.get(), value_name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_BINARY) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> data(bytes);
    if (bytes > 0 && RegQueryValueExW(key.get(), value_name, nullptr, &type, data.data(), &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return data;
}

void set_string(HKEY key, const wchar_t* name, std::wstring_view value) {
    const std::wstring terminated(value);
    const auto bytes = static_cast<DWORD>((terminated.size() + 1) * sizeof(wchar_t));
    const auto status = RegSetValueExW(key, name, 0, REG_SZ,
                                       reinterpret_cast<const BYTE*>(terminated.c_str()), bytes);
    if (status != ERROR_SUCCESS) {
        throw std::runtime_error("Unable to write registry string value");
    }
}

void set_words(HKEY key, const wchar_t* name, std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint32_t> buffer(words);
    const auto status = RegSetValueExW(key, name, 0, REG_BINARY,
                                       reinterpret_cast<const BYTE*>(buffer.data()),
                                       static_cast<DWORD>(buffer.size() * sizeof(std::uint32_t)));
    if (status != ERROR_SUCCESS) {
        throw std::runtime_error("Unable to write registry binary value");
    }
}

bool equals_ignore_case(std::wstring_view left, std::wstring_view right) noexcept {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool contains_ignore_case(std::wstring_view haystack, std::wstring_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (equals_ignore_case(haystack.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

std::wstring resolve_com_server(const std::wstring& clsid) {
    if (clsid.empty()) {
        return {};
    }
    const std::wstring path = L"Software\\Classes\\CLSID\\" + clsid + L"\\InprocServer32";
    for (const auto& [root, view] : std::array<std::pair<HKEY, REGSAM>, 4>{
             std::pair{HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY},
             std::pair{HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY},
             std::pair{HKEY_CURRENT_USER, KEY_WOW64_64KEY},
             std::pair{HKEY_CURRENT_USER, KEY_WOW64_32KEY}}) {
        if (const auto server = query_string(root, path.c_str(), L"", view); server && !server->empty()) {
            return *server;
        }
    }
    return {};
}

std::wstring escape_reg_string(std::wstring_view text) {
    std::wstring escaped;
    escaped.reserve(text.size() + 8);
    for (const wchar_t ch : text) {
        if (ch == L'\\' || ch == L'"') {
            escaped += L'\\';
        }
        escaped += ch;
    }
    return escaped;
}

void write_hex_bytes(std::wostringstream& out, const std::uint8_t* data, DWORD size) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (DWORD i = 0; i < size; ++i) {
        if (i > 0) {
            out << L',';
        }
        out << digits[data[i] >> 4] << digits[data[i] & 0xf];
    }
}

void format_reg_node(const RegExportNode& node, std::wostringstream& out) {
    out << L'[' << node.full_path << L"]\r\n";
    for (const auto& entry : node.values) {
        if (entry.name.empty()) {
            out << L"@=";
        } else {
            out << L'"' << escape_reg_string(entry.name) << L"\"=";
        }
        if (entry.type == REG_SZ) {
            const auto* chars = reinterpret_cast<const wchar_t*>(entry.data.data());
            const auto wchar_count = entry.data.size() / sizeof(wchar_t);
            const auto actual_len = wchar_count ? wcsnlen(chars, wchar_count) : 0;
            out << L'"' << escape_reg_string(std::wstring_view(chars, actual_len)) << L"\"\r\n";
        } else if (entry.type == REG_DWORD && entry.data.size() == sizeof(DWORD)) {
            DWORD word = 0;
            std::memcpy(&word, entry.data.data(), sizeof(word));
            out << L"dword:" << std::hex << std::setw(8) << std::setfill(L'0') << word << std::dec << L"\r\n";
        } else if (entry.type == REG_BINARY) {
            out << L"hex:";
            write_hex_bytes(out, entry.data.data(), static_cast<DWORD>(entry.data.size()));
            out << L"\r\n";
        } else {
            out << L"hex(" << std::hex << entry.type << std::dec << L"):";
            write_hex_bytes(out, entry.data.data(), static_cast<DWORD>(entry.data.size()));
            out << L"\r\n";
        }
    }
    out << L"\r\n";
    for (const auto& child : node.children) {
        format_reg_node(child, out);
    }
}

RegExportNode read_reg_tree(HKEY key, const std::wstring& full_path) {
    RegExportNode node{full_path, {}, {}};

    DWORD subkey_count = 0;
    DWORD max_subkey_len = 0;
    DWORD value_count = 0;
    DWORD max_value_name_len = 0;
    DWORD max_value_bytes = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkey_count, &max_subkey_len, nullptr,
                         &value_count, &max_value_name_len, &max_value_bytes, nullptr, nullptr) != ERROR_SUCCESS) {
        throw std::runtime_error("Unable to inspect registry key for backup");
    }

    std::wstring value_name(max_value_name_len + 1, L'\0');
    std::vector<std::uint8_t> value_data(max_value_bytes);
    for (DWORD i = 0; i < value_count; ++i) {
        DWORD name_len = max_value_name_len + 1;
        DWORD data_len = max_value_bytes;
        DWORD type = 0;
        const auto status = RegEnumValueW(key, i, value_name.data(), &name_len, nullptr,
                                          &type, data_len ? value_data.data() : nullptr, &data_len);
        if (status != ERROR_SUCCESS) {
            throw std::runtime_error("Unable to enumerate registry values for backup");
        }
        node.values.push_back({
            std::wstring(value_name.data(), name_len),
            type,
            std::vector<std::uint8_t>(value_data.begin(), value_data.begin() + static_cast<std::ptrdiff_t>(data_len)),
        });
    }

    std::wstring subkey_name(max_subkey_len + 1, L'\0');
    for (DWORD i = 0; i < subkey_count; ++i) {
        DWORD name_len = max_subkey_len + 1;
        if (RegEnumKeyExW(key, i, subkey_name.data(), &name_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            throw std::runtime_error("Unable to enumerate registry subkeys for backup");
        }
        const std::wstring child_name(subkey_name.data(), name_len);
        const auto child = open_key(key, child_name.c_str(), KEY_READ);
        if (!child) {
            throw std::runtime_error("Unable to open registry subkey for backup");
        }
        node.children.push_back(read_reg_tree(child.get(), full_path + L"\\" + child_name));
    }
    return node;
}

void remove_owned_key(HKEY root, const wchar_t* subkey, const wchar_t* value_name, REGSAM view) noexcept {
    const auto owner = query_string(root, subkey, value_name, view);
    if (owner && equals_ignore_case(*owner, driver_class_text)) {
        {
            const auto key = open_key(root, subkey, DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | view);
            if (key) {
                RegDeleteTreeW(key.get(), nullptr);
            }
        }
        RegDeleteKeyExW(root, subkey, view, 0);
    }
}

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::wstring timestamp_string() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);
    std::wostringstream out;
    out << std::put_time(&local, L"%Y%m%d-%H%M%S");
    return out.str();
}

void write_effects(HKEY driver) {
    struct EffectDef {
        const wchar_t* guid;
        const wchar_t* name;
        std::uint32_t id;
        std::uint32_t category;
        std::uint32_t capabilities;
        std::uint32_t dynamic_params;
    };
    constexpr std::array<EffectDef, 9> effects{{
        {L"{13541C20-8E33-11D0-9AD0-00A0C9A06E35}", L"Constant Force", 0,  1, 0x8600u, 0x3c5u},
        {L"{13541C21-8E33-11D0-9AD0-00A0C9A06E35}", L"Ramp Force",     1,  2, 0x8600u, 0x3c5u},
        {L"{13541C22-8E33-11D0-9AD0-00A0C9A06E35}", L"Square",         2,  3, 0x8600u, 0x3c5u},
        {L"{13541C23-8E33-11D0-9AD0-00A0C9A06E35}", L"Sine",           3,  3, 0x8600u, 0x3c5u},
        {L"{13541C24-8E33-11D0-9AD0-00A0C9A06E35}", L"Triangle",       4,  3, 0x8600u, 0x3c5u},
        {L"{13541C25-8E33-11D0-9AD0-00A0C9A06E35}", L"Sawtooth Up",    5,  3, 0x8600u, 0x3c5u},
        {L"{13541C26-8E33-11D0-9AD0-00A0C9A06E35}", L"Sawtooth Down",  6,  3, 0x8600u, 0x3c5u},
        {L"{13541C27-8E33-11D0-9AD0-00A0C9A06E35}", L"Spring",         7,  4, 0xf800u, 0x345u},
        {L"{13541C2A-8E33-11D0-9AD0-00A0C9A06E35}", L"Friction",      10,  4, 0xf800u, 0x345u},
    }};
    if (const auto existing = open_key(driver, L"Effects",
                                       DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_WOW64_64KEY)) {
        RegDeleteTreeW(existing.get(), nullptr);
    }
    for (const auto& effect : effects) {
        const std::wstring sub = std::wstring(L"Effects\\") + effect.guid;
        const auto eff_key = create_key(driver, sub.c_str(), KEY_WRITE | KEY_WOW64_64KEY);
        set_string(eff_key.get(), L"", effect.name);
        set_words(eff_key.get(), L"Attributes", {
            effect.id,
            effect.category | effect.capabilities,
            effect.dynamic_params | 0x20u,
            effect.dynamic_params,
            0x10u,
        });
    }
}

} // namespace

bool is_process_elevated() noexcept {
    HANDLE token_raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_raw)) {
        return false;
    }
    struct CloseTok {
        HANDLE h;
        ~CloseTok() { CloseHandle(h); }
    } token{token_raw};
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    return GetTokenInformation(token.h, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE &&
           elevation.TokenIsElevated != 0;
}

bool validate_driver_dll(const std::filesystem::path& path, std::uint16_t expected_machine) noexcept {
    try {
        if (!std::filesystem::is_regular_file(path) ||
            !equals_ignore_case(path.filename().wstring(), L"LibreWheelFF.dll")) {
            return false;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return false;
        }
        std::uint16_t mz = 0;
        input.read(reinterpret_cast<char*>(&mz), sizeof(mz));
        if (!input || mz != 0x5a4du) {
            return false;
        }
        input.seekg(0x3c);
        std::uint32_t pe_offset = 0;
        input.read(reinterpret_cast<char*>(&pe_offset), sizeof(pe_offset));
        if (!input) {
            return false;
        }
        input.seekg(pe_offset);
        std::uint32_t pe_sig = 0;
        std::uint16_t machine = 0;
        input.read(reinterpret_cast<char*>(&pe_sig), sizeof(pe_sig));
        input.read(reinterpret_cast<char*>(&machine), sizeof(machine));
        return static_cast<bool>(input) && pe_sig == 0x00004550u && machine == expected_machine;
    } catch (...) {
        return false;
    }
}

bool has_oem_conflict(bool machine_hive) noexcept {
    const HKEY root = machine_hive ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    if (!key_exists(root, force_feedback_key_path)) {
        return false;
    }
    const auto clsid = query_string(root, force_feedback_key_path, L"CLSID");
    return !clsid || !equals_ignore_case(*clsid, driver_class_text);
}

std::vector<ConflictInfo> detect_oem_conflicts() {
    std::vector<ConflictInfo> conflicts;
    for (const bool machine_hive : {true, false}) {
        if (!has_oem_conflict(machine_hive)) {
            continue;
        }
        const HKEY root = machine_hive ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
        const auto clsid = query_string(root, force_feedback_key_path, L"CLSID").value_or(L"");
        const auto server = resolve_com_server(clsid);
        const bool is_logitech = equals_ignore_case(clsid, L"{1ED6DDBB-0401-4498-A093-7D249203200C}") ||
            contains_ignore_case(server, L"Logitech") || contains_ignore_case(server, L"LGHUB") ||
            contains_ignore_case(server, L"LCore") || contains_ignore_case(server, L"lg_joy");
        conflicts.push_back({machine_hive, clsid, server, is_logitech});
    }
    return conflicts;
}

bool is_librewheel_registered() noexcept {
    const auto clsid = query_string(HKEY_CURRENT_USER, force_feedback_key_path, L"CLSID");
    return clsid && equals_ignore_case(*clsid, driver_class_text);
}

std::wstring format_reg_document(const RegExportNode& root) {
    std::wostringstream content;
    content << L"Windows Registry Editor Version 5.00\r\n\r\n";
    format_reg_node(root, content);
    return content.str();
}

std::filesystem::path export_reg_key(HKEY root, const wchar_t* root_name, const wchar_t* subkey,
                                     const std::filesystem::path& output_file) {
    const auto key = open_key(root, subkey, KEY_READ | KEY_WOW64_64KEY);
    if (!key) {
        throw std::runtime_error("Registry key to back up was not found");
    }
    const auto tree = read_reg_tree(key.get(), std::wstring(root_name) + L"\\" + subkey);
    const std::wstring text = format_reg_document(tree);

    if (output_file.has_parent_path()) {
        std::filesystem::create_directories(output_file.parent_path());
    }
    std::ofstream output(output_file, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Unable to create registry backup file");
    }
    const std::uint16_t bom = 0xfeffu;
    output.write(reinterpret_cast<const char*>(&bom), sizeof(bom));
    output.write(reinterpret_cast<const char*>(text.data()),
                 static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
    output.close();
    if (!output) {
        throw std::runtime_error("Unable to write registry backup file");
    }
    return output_file;
}

std::optional<std::filesystem::path> backup_and_remove_conflict(bool machine_hive,
                                                                const std::filesystem::path& backup_dir) {
    if (!has_oem_conflict(machine_hive)) {
        return std::nullopt;
    }
    if (machine_hive && !is_process_elevated()) {
        throw std::runtime_error(
            "Removing a machine-wide (HKLM) Logitech/OEM G29 registration requires Administrator privileges.");
    }
    const HKEY root = machine_hive ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    const wchar_t* root_name = machine_hive ? L"HKEY_LOCAL_MACHINE" : L"HKEY_CURRENT_USER";
    const wchar_t* hive_tag = machine_hive ? L"HKLM" : L"HKCU";

    std::filesystem::create_directories(backup_dir);
    const auto stamp = timestamp_string();
    auto backup_file = backup_dir / (std::wstring(L"G29-OEMForceFeedback-") + hive_tag + L"-" + stamp + L".reg");
    for (unsigned suffix = 1; std::filesystem::exists(backup_file); ++suffix) {
        backup_file = backup_dir / (std::wstring(L"G29-OEMForceFeedback-") + hive_tag + L"-" + stamp +
                                    L"-" + std::to_wstring(suffix) + L".reg");
    }
    export_reg_key(root, root_name, force_feedback_key_path, backup_file);

    LSTATUS tree_status = ERROR_SUCCESS;
    {
        const auto key = open_key(root, force_feedback_key_path,
                                  DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_WOW64_64KEY);
        if (!key) {
            throw std::runtime_error("Unable to open conflicting OEMForceFeedback key for removal");
        }
        tree_status = RegDeleteTreeW(key.get(), nullptr);
    }
    const auto key_status = RegDeleteKeyExW(root, force_feedback_key_path, KEY_WOW64_64KEY, 0);
    if ((tree_status != ERROR_SUCCESS && tree_status != ERROR_FILE_NOT_FOUND) ||
        (key_status != ERROR_SUCCESS && key_status != ERROR_FILE_NOT_FOUND)) {
        throw std::runtime_error("Unable to delete conflicting OEMForceFeedback registry key");
    }
    return backup_file;
}

void install_or_switch_registration(const std::filesystem::path& dll64,
                                    const std::filesystem::path& dll32,
                                    bool validate_only,
                                    bool allow_existing_oem) {
    if (!validate_only) {
        if (!validate_driver_dll(dll64, 0x8664u)) {
            throw std::runtime_error("Expected a valid 64-bit LibreWheelFF.dll");
        }
        if (!validate_driver_dll(dll32, 0x014cu)) {
            throw std::runtime_error("Expected a valid 32-bit LibreWheelFF.dll");
        }
    }

    const bool switching = is_librewheel_registered();
    if (has_oem_conflict(true) && !(validate_only && allow_existing_oem)) {
        throw std::runtime_error("A machine-wide Logitech/OEM G29 force-feedback registration is present.");
    }

    if (switching) {
        const auto owner64 = query_string(HKEY_CURRENT_USER, com_class_key_path, owner_value_name, KEY_WOW64_64KEY);
        const auto owner32 = query_string(HKEY_CURRENT_USER, com_class_key_path, owner_value_name, KEY_WOW64_32KEY);
        if (!owner64 || !equals_ignore_case(*owner64, driver_class_text) ||
            !owner32 || !equals_ignore_case(*owner32, driver_class_text)) {
            throw std::runtime_error("Existing LibreWheel COM registration is incomplete.");
        }
        const std::wstring server_path = std::wstring(com_class_key_path) + L"\\InprocServer32";
        const REGSAM access = validate_only ? KEY_READ : (KEY_READ | KEY_WRITE);
        const auto server64 = open_key(HKEY_CURRENT_USER, server_path.c_str(), access | KEY_WOW64_64KEY);
        const auto server32 = open_key(HKEY_CURRENT_USER, server_path.c_str(), access | KEY_WOW64_32KEY);
        if (!server64 || !server32) {
            throw std::runtime_error("LibreWheel is missing one of its registered in-process COM servers.");
        }
        if (validate_only) {
            return;
        }
        const auto previous64 = query_string(HKEY_CURRENT_USER, server_path.c_str(), L"", KEY_WOW64_64KEY);
        set_string(server64.get(), L"", dll64.wstring());
        try {
            set_string(server32.get(), L"", dll32.wstring());
        } catch (...) {
            if (previous64) {
                set_string(server64.get(), L"", *previous64);
            }
            throw;
        }
        if (const auto driver = open_key(HKEY_CURRENT_USER, force_feedback_key_path, KEY_WRITE | KEY_WOW64_64KEY)) {
            set_words(driver.get(), L"Attributes", {0u, 2000u, 2000u});
            write_effects(driver.get());
        }
        return;
    }

    if (has_oem_conflict(false) && !(validate_only && allow_existing_oem)) {
        throw std::runtime_error("A per-user Logitech/OEM G29 force-feedback registration is present.");
    }
    if (key_exists(HKEY_CURRENT_USER, com_class_key_path, KEY_WOW64_64KEY) ||
        key_exists(HKEY_CURRENT_USER, com_class_key_path, KEY_WOW64_32KEY)) {
        throw std::runtime_error("The LibreWheel COM class is already present in an unexpected state.");
    }

    const bool reuse_axis = key_exists(HKEY_CURRENT_USER, axis0_key_path, KEY_WOW64_64KEY);
    if (reuse_axis) {
        const auto attrs = query_binary(HKEY_CURRENT_USER, axis0_key_path, L"Attributes", KEY_WOW64_64KEY);
        const auto ff_attrs = query_binary(HKEY_CURRENT_USER, axis0_key_path, L"FFAttributes", KEY_WOW64_64KEY);
        if (!attrs || attrs->size() != 4 || read_u32_le(*attrs, 0) != 0x8101u ||
            !ff_attrs || ff_attrs->size() != 8 || read_u32_le(*ff_attrs, 0) == 0 ||
            read_u32_le(*ff_attrs, 4) == 0) {
            throw std::runtime_error("Existing steering axis registry entry is incompatible with G29.");
        }
    }
    if (validate_only) {
        return;
    }

    struct Created {
        std::wstring path;
        REGSAM view;
    };
    std::vector<Created> created;
    try {
        for (const auto& [view, dll] : std::array<std::pair<REGSAM, std::filesystem::path>, 2>{
                 std::pair{KEY_WOW64_64KEY, dll64}, std::pair{KEY_WOW64_32KEY, dll32}}) {
            const auto key = create_key(HKEY_CURRENT_USER, com_class_key_path, KEY_WRITE | view);
            created.push_back({com_class_key_path, view});
            set_string(key.get(), owner_value_name, driver_class_text);
            const auto server = create_key(key.get(), L"InprocServer32", KEY_WRITE | view);
            set_string(server.get(), L"", dll.wstring());
            set_string(server.get(), L"ThreadingModel", L"Both");
        }

        if (!reuse_axis) {
            const auto axis = create_key(HKEY_CURRENT_USER, axis0_key_path, KEY_WRITE | KEY_WOW64_64KEY);
            created.push_back({axis0_key_path, KEY_WOW64_64KEY});
            set_string(axis.get(), owner_value_name, driver_class_text);
            set_string(axis.get(), L"", L"Steering");
            set_words(axis.get(), L"Attributes", {0x8101u});
            set_words(axis.get(), L"FFAttributes", {10u, 128u});
        }

        const auto driver = create_key(HKEY_CURRENT_USER, force_feedback_key_path, KEY_WRITE | KEY_WOW64_64KEY);
        created.push_back({force_feedback_key_path, KEY_WOW64_64KEY});
        set_words(driver.get(), L"Attributes", {0u, 2000u, 2000u});
        write_effects(driver.get());
        // Publish the product-to-class association last.
        set_string(driver.get(), L"CLSID", driver_class_text);
    } catch (...) {
        for (auto it = created.rbegin(); it != created.rend(); ++it) {
            {
                const auto key = open_key(HKEY_CURRENT_USER, it->path.c_str(),
                                          DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | it->view);
                if (key) {
                    RegDeleteTreeW(key.get(), nullptr);
                }
            }
            RegDeleteKeyExW(HKEY_CURRENT_USER, it->path.c_str(), it->view, 0);
        }
        throw;
    }
}

void remove_registration(const std::filesystem::path& expected_dll64,
                         const std::filesystem::path& expected_dll32) {
    if (!expected_dll64.empty() || !expected_dll32.empty()) {
        const std::wstring server_path = std::wstring(com_class_key_path) + L"\\InprocServer32";
        const auto current64 = query_string(HKEY_CURRENT_USER, server_path.c_str(), L"", KEY_WOW64_64KEY);
        const auto current32 = query_string(HKEY_CURRENT_USER, server_path.c_str(), L"", KEY_WOW64_32KEY);
        if (!current64 || !current32 ||
            !equals_ignore_case(*current64, expected_dll64.wstring()) ||
            !equals_ignore_case(*current32, expected_dll32.wstring())) {
            return;
        }
    }
    remove_owned_key(HKEY_CURRENT_USER, force_feedback_key_path, L"CLSID", KEY_WOW64_64KEY);
    remove_owned_key(HKEY_CURRENT_USER, axis0_key_path, owner_value_name, KEY_WOW64_64KEY);
    remove_owned_key(HKEY_CURRENT_USER, com_class_key_path, owner_value_name, KEY_WOW64_64KEY);
    remove_owned_key(HKEY_CURRENT_USER, com_class_key_path, owner_value_name, KEY_WOW64_32KEY);
}

} // namespace lw::win
