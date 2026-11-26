// SPDX-License-Identifier: GPL-3.0-or-later
#include "payload_archive.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Unable to open input file: " + path.string());
    }
    const auto size = input.tellg();
    if (size < 0) {
        throw std::runtime_error("Unable to size input file: " + path.string());
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !input.seekg(0).read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("Unable to read input file: " + path.string());
    }
    return bytes;
}

std::string utf8_from_wide(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        throw std::runtime_error("Unable to encode payload path as UTF-8");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 3) {
            std::wcerr << L"Usage: pack_payload <output.bin> <app|config>:<relative_path>=<source_file> ...\n";
            return 1;
        }
        std::vector<lw::win::PayloadEntry> entries;
        for (int index = 2; index < argc; ++index) {
            const std::wstring_view spec(argv[index]);
            const auto colon = spec.find(L':');
            const auto equals = spec.find(L'=', colon == spec.npos ? 0 : colon + 1);
            if (colon == spec.npos || equals == spec.npos) {
                throw std::runtime_error("Invalid payload entry spec");
            }
            const auto kind = spec.substr(0, colon);
            lw::win::PayloadTarget target = lw::win::PayloadTarget::app;
            if (kind == L"app") {
                target = lw::win::PayloadTarget::app;
            } else if (kind == L"config") {
                target = lw::win::PayloadTarget::config_if_missing;
            } else {
                throw std::runtime_error("Payload target must be 'app' or 'config'");
            }
            const auto relative = utf8_from_wide(spec.substr(colon + 1, equals - colon - 1));
            const std::filesystem::path source(spec.substr(equals + 1));
            entries.push_back({relative, target, read_file_bytes(source)});
        }

        const auto packed = lw::win::pack_payload(entries);
        const std::filesystem::path output_path(argv[1]);
        if (output_path.has_parent_path()) {
            std::filesystem::create_directories(output_path.parent_path());
        }
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("Unable to write packed payload file");
        }
        std::cout << "Packed " << entries.size() << " files into " << packed.size() << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "pack_payload: " << error.what() << '\n';
        return 1;
    }
}
