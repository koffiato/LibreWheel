// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>
#include <compressapi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lw::win {

enum class PayloadTarget : std::uint32_t {
    app = 0,               // Always install/overwrite under install directory.
    config_if_missing = 1, // Install under %LOCALAPPDATA%\LibreWheel only if absent; preserve on uninstall.
};

struct PayloadEntry {
    std::string relative_path; // UTF-8 relative path with '/' or '\\' separators, no '..' or drive prefix.
    PayloadTarget target = PayloadTarget::app;
    std::vector<std::uint8_t> data;
};

namespace detail {

inline std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xffffffffu;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

template<class T>
void append_le(std::vector<std::uint8_t>& out, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
    }
}

template<class T>
T read_le(std::span<const std::uint8_t> bytes, std::size_t& offset) {
    if (offset + sizeof(T) > bytes.size()) {
        throw std::runtime_error("Truncated LibreWheel installer payload");
    }
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(bytes[offset + i]) << (i * 8);
    }
    offset += sizeof(T);
    return value;
}

inline bool is_safe_relative_path(std::string_view relative) noexcept {
    if (relative.empty() || relative.size() > 260) {
        return false;
    }
    if (relative.front() == '/' || relative.front() == '\\' || relative.find(':') != relative.npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= relative.size()) {
        const auto sep = relative.find_first_of("/\\", start);
        const auto segment = relative.substr(start, sep == relative.npos ? relative.size() - start : sep - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (sep == relative.npos) {
            break;
        }
        start = sep + 1;
    }
    return true;
}

constexpr std::array<std::uint8_t, 4> archive_magic{'L', 'W', 'P', '1'};
constexpr std::array<std::uint8_t, 4> container_magic{'L', 'W', 'Z', '1'};

} // namespace detail

inline std::vector<std::uint8_t> pack_payload(const std::vector<PayloadEntry>& entries) {
    std::vector<std::uint8_t> raw;
    raw.insert(raw.end(), detail::archive_magic.begin(), detail::archive_magic.end());
    detail::append_le<std::uint32_t>(raw, static_cast<std::uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        if (!detail::is_safe_relative_path(entry.relative_path)) {
            throw std::runtime_error("Unsafe payload path: " + entry.relative_path);
        }
        detail::append_le<std::uint32_t>(raw, static_cast<std::uint32_t>(entry.target));
        detail::append_le<std::uint16_t>(raw, static_cast<std::uint16_t>(entry.relative_path.size()));
        raw.insert(raw.end(), entry.relative_path.begin(), entry.relative_path.end());
        detail::append_le<std::uint32_t>(raw, static_cast<std::uint32_t>(entry.data.size()));
        detail::append_le<std::uint32_t>(raw, detail::crc32(entry.data));
        raw.insert(raw.end(), entry.data.begin(), entry.data.end());
    }

    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) {
        throw std::runtime_error("Unable to initialize Windows LZMS compressor");
    }
    struct CloseComp {
        COMPRESSOR_HANDLE handle;
        ~CloseComp() { CloseCompressor(handle); }
    } close_comp{compressor};

    SIZE_T compressed_size = 0;
    if (!Compress(compressor, raw.data(), raw.size(), nullptr, 0, &compressed_size) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        throw std::runtime_error("Unable to size compressed installer payload");
    }
    std::vector<std::uint8_t> compressed(compressed_size);
    if (!Compress(compressor, raw.data(), raw.size(), compressed.data(), compressed.size(), &compressed_size)) {
        throw std::runtime_error("Unable to compress installer payload");
    }
    compressed.resize(compressed_size);

    std::vector<std::uint8_t> container;
    container.reserve(12 + compressed.size());
    container.insert(container.end(), detail::container_magic.begin(), detail::container_magic.end());
    detail::append_le<std::uint32_t>(container, static_cast<std::uint32_t>(raw.size()));
    detail::append_le<std::uint32_t>(container, detail::crc32(raw));
    container.insert(container.end(), compressed.begin(), compressed.end());
    return container;
}

inline std::vector<PayloadEntry> unpack_payload(std::span<const std::uint8_t> container) {
    if (container.size() < 12 ||
        std::memcmp(container.data(), detail::container_magic.data(), detail::container_magic.size()) != 0) {
        throw std::runtime_error("Invalid installer payload header");
    }
    std::size_t offset = 4;
    const auto raw_size = detail::read_le<std::uint32_t>(container, offset);
    const auto raw_crc = detail::read_le<std::uint32_t>(container, offset);
    if (raw_size == 0 || raw_size > 64u * 1024u * 1024u) {
        throw std::runtime_error("Installer payload size is out of bounds");
    }

    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor)) {
        throw std::runtime_error("Unable to initialize Windows LZMS decompressor");
    }
    struct CloseDecomp {
        DECOMPRESSOR_HANDLE handle;
        ~CloseDecomp() { CloseDecompressor(handle); }
    } close_decomp{decompressor};

    std::vector<std::uint8_t> raw(raw_size);
    SIZE_T actual_size = 0;
    if (!Decompress(decompressor, container.data() + offset, container.size() - offset,
                    raw.data(), raw.size(), &actual_size) || actual_size != raw_size) {
        throw std::runtime_error("Unable to decompress installer payload");
    }
    if (detail::crc32(raw) != raw_crc) {
        throw std::runtime_error("Installer payload checksum mismatch");
    }

    if (raw.size() < 8 ||
        std::memcmp(raw.data(), detail::archive_magic.data(), detail::archive_magic.size()) != 0) {
        throw std::runtime_error("Invalid installer archive signature");
    }
    std::size_t raw_offset = 4;
    const auto count = detail::read_le<std::uint32_t>(raw, raw_offset);
    if (count > 256) {
        throw std::runtime_error("Unexpected file count in installer payload");
    }

    std::vector<PayloadEntry> entries;
    entries.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto target_value = detail::read_le<std::uint32_t>(raw, raw_offset);
        if (target_value > static_cast<std::uint32_t>(PayloadTarget::config_if_missing)) {
            throw std::runtime_error("Invalid payload target flag");
        }
        const auto path_len = detail::read_le<std::uint16_t>(raw, raw_offset);
        if (raw_offset + path_len > raw.size()) {
            throw std::runtime_error("Truncated payload file path");
        }
        std::string relative(reinterpret_cast<const char*>(raw.data() + raw_offset), path_len);
        raw_offset += path_len;
        if (!detail::is_safe_relative_path(relative)) {
            throw std::runtime_error("Unsafe path in installer payload: " + relative);
        }
        const auto data_len = detail::read_le<std::uint32_t>(raw, raw_offset);
        const auto expected_crc = detail::read_le<std::uint32_t>(raw, raw_offset);
        if (raw_offset + data_len > raw.size()) {
            throw std::runtime_error("Truncated payload file data");
        }
        std::vector<std::uint8_t> data(raw.begin() + static_cast<std::ptrdiff_t>(raw_offset),
                                       raw.begin() + static_cast<std::ptrdiff_t>(raw_offset + data_len));
        raw_offset += data_len;
        if (detail::crc32(data) != expected_crc) {
            throw std::runtime_error("Corrupted payload file: " + relative);
        }
        entries.push_back({std::move(relative), static_cast<PayloadTarget>(target_value), std::move(data)});
    }
    if (raw_offset != raw.size()) {
        throw std::runtime_error("Unexpected trailing bytes in installer payload");
    }
    return entries;
}

} // namespace lw::win
