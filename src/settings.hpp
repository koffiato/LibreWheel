// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <istream>
#include <optional>
#include <string>
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
    bool log_ffb = false;
};

struct LutSelection {
    std::filesystem::path file;
    bool enabled = false;
};

Settings load_settings(const std::filesystem::path& path);
bool reload_lut(const std::filesystem::path& path, std::filesystem::file_time_type& last_loaded,
                std::optional<Lut>& lut);
LutSelection read_lut_selection(std::istream& ini);
std::string write_lut_selection(std::string ini, const LutSelection& selection);
double corrected_force(double mixed_force, const Settings& settings) noexcept;

} // namespace lw
