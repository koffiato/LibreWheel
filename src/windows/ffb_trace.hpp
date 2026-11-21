// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <locale>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>

namespace lw::win {

inline constexpr std::size_t trace_capacity = 131072;

inline std::chrono::microseconds trace_time() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
}

template<std::size_t N>
void write_trace_bytes(std::ostream& output, const std::array<std::uint8_t, N>& bytes, std::size_t size = N) {
    constexpr char hex[] = "0123456789abcdef";
    std::array<char, N * 2> encoded;
    for (std::size_t i = 0; i < size; ++i) {
        encoded[i * 2] = hex[bytes[i] >> 4];
        encoded[i * 2 + 1] = hex[bytes[i] & 0xf];
    }
    output.write(encoded.data(), static_cast<std::streamsize>(size * 2));
}

template<class Row>
class TraceBuffer {
public:
    void add(const Row& row) noexcept {
        const auto index = static_cast<std::size_t>(count_ % trace_capacity);
        auto& block = blocks_[index / block_size];
        if (!block) {
            block.reset(new (std::nothrow) Row[block_size]);
            if (!block) {
                ++skipped_;
                return;
            }
        }
        block[index % block_size] = row;
        ++count_;
    }

    void write(const std::filesystem::path& file, std::string_view header) const {
        std::ofstream output(file, std::ios::binary);
        output.imbue(std::locale::classic());
        output.precision(10);
        const auto first = count_ > trace_capacity ? count_ - trace_capacity : 0;
        output << "# dropped_rows=" << first + skipped_ << '\n' << header << '\n';
        for (auto i = first; i < count_; ++i) {
            const auto index = static_cast<std::size_t>(i % trace_capacity);
            output << blocks_[index / block_size][index % block_size] << '\n';
        }
        output.close();
        if (!output) {
            throw std::runtime_error("Unable to save LibreWheel force-feedback trace");
        }
    }

private:
    static constexpr std::size_t block_size = 512;
    static_assert(trace_capacity % block_size == 0);
    std::array<std::unique_ptr<Row[]>, trace_capacity / block_size> blocks_{};
    std::uint64_t count_ = 0;
    std::uint64_t skipped_ = 0;
};

} // namespace lw::win
