// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lw {
namespace {

std::string_view trim(std::string_view line) {
    const auto first = line.find_first_not_of(" \t\r");
    if (first == line.npos) {
        return {};
    }
    return line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
}

std::string_view content(const std::string& line, unsigned number) {
    std::string_view value = line;
    if (number == 1 && value.starts_with("\xef\xbb\xbf")) {
        value.remove_prefix(3);
    }
    return trim(value);
}

template<class T> T number(std::string_view field, std::string_view name) {
    const auto value = trim(field);
    T result{};
    if (!value.empty()) {
        const auto* end = value.data() + value.size();
        const auto [next, status] = std::from_chars(value.data(), end, result);
        if (status == std::errc{} && next == end && std::isfinite(result)) {
            return result;
        }
    }
    throw std::runtime_error("Invalid number in " + std::string(name));
}

bool boolean(std::string_view value) {
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    throw std::runtime_error("Expected true or false");
}

} // namespace

Lut Lut::read(std::istream& csv) {
    Lut lut;
    bool header = false;
    std::string line;
    unsigned row = 0;
    while (std::getline(csv, line)) {
        const auto entry = content(line, ++row);
        if (entry.empty() || entry.starts_with('#')) {
            continue;
        }
        if (!header && lut.points_.empty() && entry == "input,output") {
            header = true;
            continue;
        }
        const auto split = entry.find(',');
        if (split == entry.npos || lut.points_.size() >= 4096) {
            throw std::runtime_error("Invalid LUT row " + std::to_string(row));
        }
        const auto label = "LUT row " + std::to_string(row);
        const double x = number<double>(entry.substr(0, split), label);
        const double y = number<double>(entry.substr(split + 1), label);
        if (x < 0 || x > 1 || y < 0 || y > 1 ||
            (!lut.points_.empty() && x <= lut.points_.back().first)) {
            throw std::runtime_error("Out-of-range or unordered " + label);
        }
        lut.points_.emplace_back(x, y);
    }
    if (!csv.eof() || lut.points_.size() < 2 ||
        lut.points_.front() != std::pair{0.0, 0.0} || lut.points_.back().first != 1.0) {
        throw std::runtime_error("LUT must start at 0,0 and cover the full input domain through 1");
    }
    return lut;
}

double Lut::apply(double force) const noexcept {
    if (!std::isfinite(force)) {
        return 0;
    }
    const auto magnitude = std::min(1.0, std::abs(force));
    if (points_.empty() || magnitude == 0) {
        return std::copysign(magnitude, force);
    }
    const auto upper = std::ranges::lower_bound(points_, magnitude, {}, &std::pair<double, double>::first);
    const auto& [from, low] = *(upper - 1);
    const auto& [to, high] = *upper;
    return std::copysign(std::lerp(low, high, (magnitude - from) / (to - from)), force);
}

Settings load_settings(const std::filesystem::path& path) {
    Settings settings;
    std::ifstream file(path);
    if (!file) {
        if (!std::filesystem::exists(path)) {
            return settings;
        }
        throw std::runtime_error("Unable to open settings file");
    }
    std::set<std::string, std::less<>> keys;
    std::filesystem::path lut_path;
    bool lut_enabled = false;
    std::string line;
    unsigned line_number = 0;
    while (std::getline(file, line)) {
        const auto entry = content(line, ++line_number);
        if (entry.empty() || entry.starts_with('#')) {
            continue;
        }
        const auto split = entry.find('=');
        if (split == entry.npos) {
            throw std::runtime_error("Expected key=value on line " + std::to_string(line_number));
        }
        const auto key = trim(entry.substr(0, split));
        const auto value = trim(entry.substr(split + 1));
        if (!keys.emplace(key).second) {
            throw std::runtime_error("Duplicate setting " + std::string(key));
        }
        if (key == "gain_percent") {
            const double gain = number<double>(value, key);
            if (gain < 0 || gain > 100) {
                throw std::runtime_error("gain_percent must be between 0 and 100");
            }
            settings.gain = gain / 100;
        } else if (key == "range_degrees") {
            const unsigned degrees = number<unsigned>(value, key);
            if (degrees < 40 || degrees > 900) {
                throw std::runtime_error("range_degrees must be between 40 and 900");
            }
            settings.range = degrees;
        } else if (key == "lut_enabled") {
            lut_enabled = boolean(value);
        } else if (key == "lut_file") {
            lut_path = std::filesystem::path(std::u8string(value.begin(), value.end()));
        } else if (key == "diagnostics") {
            settings.diagnostics = boolean(value);
        } else {
            throw std::runtime_error("Unknown setting " + std::string(key));
        }
    }
    if (!file.eof()) {
        throw std::runtime_error("Unable to read settings file");
    }
    if (lut_enabled) {
        if (lut_path.empty()) {
            throw std::runtime_error("lut_enabled requires lut_file");
        }
        std::ifstream csv(path.parent_path() / lut_path);
        if (!csv) {
            throw std::runtime_error("Unable to open LUT file");
        }
        settings.lut = Lut::read(csv);
    }
    return settings;
}

double corrected_force(double mixed_force, const Settings& settings) noexcept {
    if (!std::isfinite(mixed_force)) {
        return 0;
    }
    const double scaled = std::clamp(mixed_force * settings.gain, -1.0, 1.0);
    return settings.lut ? settings.lut->apply(scaled) : scaled;
}

} // namespace lw
