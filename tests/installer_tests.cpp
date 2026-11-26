// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows/payload_archive.hpp"
#include "windows/registration.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool actual, const char* description) {
    if (!actual) {
        ++failures;
        std::cerr << description << '\n';
    }
}

void test_payload_roundtrip() {
    using namespace lw::win;
    std::vector<PayloadEntry> input{
        {"LibreWheelTray.exe", PayloadTarget::app, {0x4d, 0x5a, 0x90, 0x00, 0x01, 0x02, 0x03}},
        {"x64/LibreWheelFF.dll", PayloadTarget::app, std::vector<std::uint8_t>(4096, 0x5au)},
        {"settings.ini", PayloadTarget::config_if_missing, {'g', 'a', 'i', 'n', '=', '1'}},
    };
    const auto packed = pack_payload(input);
    check(packed.size() < 4096, "LZMS payload compresses repetitive binary data");

    const auto unpacked = unpack_payload(packed);
    check(unpacked.size() == input.size(), "unpacked entry count matches");
    for (std::size_t i = 0; i < input.size() && i < unpacked.size(); ++i) {
        check(unpacked[i].relative_path == input[i].relative_path &&
              unpacked[i].target == input[i].target &&
              unpacked[i].data == input[i].data,
              "unpacked entry matches original bytes and target");
    }

    auto corrupted = packed;
    corrupted.back() ^= 0xffu;
    bool rejected_corrupt = false;
    try {
        (void)unpack_payload(corrupted);
    } catch (const std::runtime_error&) {
        rejected_corrupt = true;
    }
    check(rejected_corrupt, "corrupted LZMS payload is rejected");

    for (const char* unsafe : {"../escape.txt", "C:\\abs.txt", "\\root.txt", "a/./b", "a//b", ""}) {
        bool rejected = false;
        try {
            (void)pack_payload({{unsafe, PayloadTarget::app, {1, 2, 3}}});
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "unsafe relative payload path is rejected");
    }
}

void test_pe_validation(const std::filesystem::path& dll_path) {
    constexpr std::uint16_t native_machine = sizeof(void*) == 8 ? 0x8664u : 0x014cu;
    constexpr std::uint16_t other_machine = sizeof(void*) == 8 ? 0x014cu : 0x8664u;
    check(lw::win::validate_driver_dll(dll_path, native_machine), "built LibreWheelFF.dll matches target PE machine");
    check(!lw::win::validate_driver_dll(dll_path, other_machine), "mismatched PE architecture is rejected");
    check(!lw::win::validate_driver_dll(dll_path.parent_path() / L"missing.dll", native_machine),
          "missing DLL is rejected");
}

void test_reg_export() {
    auto wide_bytes = [](std::wstring_view text) {
        std::vector<std::uint8_t> bytes((text.size() + 1) * sizeof(wchar_t), 0);
        std::memcpy(bytes.data(), text.data(), text.size() * sizeof(wchar_t));
        return bytes;
    };
    const DWORD sample = 0x000003e8u;
    std::vector<std::uint8_t> dword_bytes(sizeof(sample));
    std::memcpy(dword_bytes.data(), &sample, sizeof(sample));

    lw::win::RegExportNode tree{
        L"HKEY_CURRENT_USER\\Software\\LibreWheelSyntheticTest",
        {
            {L"CLSID", REG_SZ, wide_bytes(L"{1ED6DDBB-0401-4498-A093-7D249203200C}")},
            {L"Sample", REG_DWORD, dword_bytes},
            {L"Attributes", REG_BINARY, {0x00, 0x01, 0x86, 0xff}},
        },
        {
            {
                L"HKEY_CURRENT_USER\\Software\\LibreWheelSyntheticTest\\Effects\\{13541C20-8E33-11D0-9AD0-00A0C9A06E35}",
                {{L"", REG_SZ, wide_bytes(L"Constant\\Force")}},
                {},
            },
        },
    };
    const std::wstring formatted = lw::win::format_reg_document(tree);
    check(formatted.find(L"Windows Registry Editor Version 5.00\r\n") == 0, "reg document has standard header");
    check(formatted.find(L"\"CLSID\"=\"{1ED6DDBB-0401-4498-A093-7D249203200C}\"") != formatted.npos,
          "reg document exports REG_SZ");
    check(formatted.find(L"\"Sample\"=dword:000003e8") != formatted.npos, "reg document exports REG_DWORD");
    check(formatted.find(L"\"Attributes\"=hex:00,01,86,ff") != formatted.npos, "reg document exports REG_BINARY");
    check(formatted.find(L"@=\"Constant\\\\Force\"") != formatted.npos,
          "reg document exports escaped default value in subkey");

    // When HKCU writes are allowed (non-sandboxed hosts), also verify live Win32 registry export end-to-end.
    const std::wstring subkey = L"Software\\LibreWheelRegExportTest-" + std::to_wstring(GetCurrentProcessId());
    HKEY root = nullptr;
    const auto create_status = RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0,
                                               KEY_WRITE | KEY_WOW64_64KEY, nullptr, &root, nullptr);
    if (create_status == ERROR_ACCESS_DENIED) {
        return;
    }
    if (create_status != ERROR_SUCCESS) {
        check(false, "create temporary HKCU test key for reg export");
        return;
    }
    struct CleanupReg {
        std::wstring path;
        ~CleanupReg() {
            HKEY raw = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
                              DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_WOW64_64KEY, &raw) == ERROR_SUCCESS) {
                RegDeleteTreeW(raw, nullptr);
                RegCloseKey(raw);
            }
            RegDeleteKeyExW(HKEY_CURRENT_USER, path.c_str(), KEY_WOW64_64KEY, 0);
        }
    } cleanup_reg{subkey};

    const std::wstring clsid = L"{1ED6DDBB-0401-4498-A093-7D249203200C}";
    RegSetValueExW(root, L"CLSID", 0, REG_SZ, reinterpret_cast<const BYTE*>(clsid.c_str()),
                   static_cast<DWORD>((clsid.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(root, L"Sample", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&sample), sizeof(sample));
    const std::uint8_t attrs[]{0x00, 0x01, 0x86, 0xff};
    RegSetValueExW(root, L"Attributes", 0, REG_BINARY, attrs, sizeof(attrs));
    RegCloseKey(root);

    const auto reg_file = std::filesystem::current_path() /
        (L"reg-export-test-" + std::to_wstring(GetCurrentProcessId()) + L".reg");
    struct CleanupFile {
        std::filesystem::path file;
        ~CleanupFile() {
            std::error_code ec;
            std::filesystem::remove(file, ec);
        }
    } cleanup_file{reg_file};

    lw::win::export_reg_key(HKEY_CURRENT_USER, L"HKEY_CURRENT_USER", subkey.c_str(), reg_file);
    std::ifstream in(reg_file, std::ios::binary | std::ios::ate);
    const auto bytes = in.tellg();
    check(in && bytes > 10 && (bytes % 2) == 0, "exported .reg file exists and has UTF-16 byte length");
    std::vector<std::uint16_t> words(static_cast<std::size_t>(bytes) / 2);
    in.seekg(0).read(reinterpret_cast<char*>(words.data()), bytes);
    check(!words.empty() && words[0] == 0xfeffu, "exported .reg file starts with UTF-16 LE BOM");
    const std::wstring text(words.begin() + 1, words.end());
    check(text.find(L"\"CLSID\"=\"{1ED6DDBB-0401-4498-A093-7D249203200C}\"") != text.npos,
          "live exported .reg file preserves REG_SZ");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    test_payload_roundtrip();
    if (argc == 2) {
        test_pe_validation(argv[1]);
    }
    test_reg_export();
    return failures != 0;
}
