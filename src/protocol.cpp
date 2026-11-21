// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol.hpp"

#include <algorithm>
#include <cmath>

namespace lw::g29 {

std::uint8_t force_byte(double force) noexcept {
    if (!std::isfinite(force)) {
        return 0x80;
    }
    // Positive physical force means increasing X. The classic byte runs the other way.
    const double bounded = std::clamp(force, -1.0, 1.0);
    const long strength = std::lround(bounded * (bounded >= 0 ? 128 : 127));
    return static_cast<std::uint8_t>(128 - strength);
}

OutputReport torque(std::uint8_t force) noexcept {
    return force == 0x80 ? OutputReport{0, 0x13} : OutputReport{0, 0x11, 0x08, force, 0x80};
}

OutputReport disable_centering() noexcept { return {0, 0xf5}; }

std::optional<OutputReport> operating_range(unsigned degrees) noexcept {
    if (degrees < 40 || degrees > 900) {
        return std::nullopt;
    }
    return OutputReport{0, 0xf8, 0x81, static_cast<std::uint8_t>(degrees),
                        static_cast<std::uint8_t>(degrees >> 8)};
}

} // namespace lw::g29
