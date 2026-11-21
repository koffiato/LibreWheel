// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <locale>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace lw::win {

inline constexpr std::size_t trace_capacity = 131072;

inline std::chrono::microseconds trace_time() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
}

inline void write_trace_bytes(std::ostream& output, const std::uint8_t* bytes, std::size_t size) {
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < size; ++i) {
        output.put(hex[bytes[i] >> 4]).put(hex[bytes[i] & 0xf]);
    }
}

template<class Row>
class TraceBuffer {
public:
    TraceBuffer() : rows_(trace_capacity) {}

    void add(const Row& row) noexcept {
        rows_[count_ % rows_.size()] = row;
        ++count_;
    }

    void write(const std::filesystem::path& file, std::string_view header) const {
        std::ofstream output(file, std::ios::binary);
        output.imbue(std::locale::classic());
        output.precision(10);
        const auto first = count_ > rows_.size() ? count_ - rows_.size() : 0;
        output << "# dropped_rows=" << first << '\n' << header << '\n';
        for (auto i = first; i < count_; ++i) {
            output << rows_[i % rows_.size()] << '\n';
        }
        output.close();
        if (!output) {
            throw std::runtime_error("Unable to save LibreWheel force-feedback trace");
        }
    }

private:
    std::vector<Row> rows_;
    std::uint64_t count_ = 0;
};

} // namespace lw::win
