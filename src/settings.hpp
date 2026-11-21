// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <istream>
#include <optional>
#include <utility>
#include <vector>

namespace lw {

class Lut {
public:
    static Lut read(std::istream& csv);
    double apply(double force) const noexcept;

private:
    std::vector<std::pair<double, double>> points_;
};

struct Settings {
    double gain = 1.0;
    std::optional<unsigned> range;
    std::optional<Lut> lut;
    bool diagnostics = false;
};

Settings load_settings(const std::filesystem::path& path);
double corrected_force(double mixed_force, const Settings& settings) noexcept;

} // namespace lw
