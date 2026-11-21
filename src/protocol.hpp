// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace lw::g29 {

inline constexpr unsigned vendor_id = 0x046d;
inline constexpr unsigned product_id = 0xc24f;
inline constexpr unsigned compatibility_product_id = 0xc294;

// C294 is shared with other wheels. Match documented G29 revision families.
constexpr bool native_switch_candidate(unsigned vendor, unsigned product, unsigned revision) noexcept {
    return vendor == vendor_id && product == compatibility_product_id &&
           ((revision & 0xfff8) == 0x1350 || (revision & 0xff00) == 0x8900);
}

// The first byte is the Win32 report ID. Pad to the collection's reported length.
inline constexpr std::array<std::uint8_t, 8> reset_mode_on_usb_reset{0, 0xf8, 0x0a, 0, 0, 0, 0, 0};
inline constexpr std::array<std::uint8_t, 8> switch_to_native_mode{0, 0xf8, 0x09, 0x05, 0x01, 0x01, 0, 0};

// Byte zero is Win32's synthetic report ID. The remaining 16 bytes go on the wire.
using OutputReport = std::array<std::uint8_t, 17>;
std::uint8_t force_byte(double force) noexcept;
OutputReport torque(std::uint8_t force) noexcept;
OutputReport disable_centering() noexcept;
std::optional<OutputReport> operating_range(unsigned degrees) noexcept;

} // namespace lw::g29
