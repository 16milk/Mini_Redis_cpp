#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cluster {

// CRC16/XMODEM as used by Redis Cluster: polynomial 0x1021, initial value
// 0x0000, no input/output reflection and no final XOR. The table is generated
// at compile time so the 256 constants cannot be mistranscribed.
namespace detail {

constexpr std::array<std::uint16_t, 256> makeCrc16Table() {
    std::array<std::uint16_t, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index) {
        auto remainder = static_cast<std::uint16_t>(index << 8);
        for (int bit = 0; bit < 8; ++bit) {
            const bool high_bit_set = (remainder & 0x8000u) != 0;
            remainder = static_cast<std::uint16_t>(remainder << 1);
            if (high_bit_set) {
                remainder = static_cast<std::uint16_t>(remainder ^ 0x1021u);
            }
        }
        table[index] = remainder;
    }
    return table;
}

inline constexpr std::array<std::uint16_t, 256> kCrc16Table = makeCrc16Table();

} // namespace detail

// Operates on raw bytes with an explicit length, so keys containing '\0' hash
// exactly like they do on a real Redis Cluster node.
constexpr std::uint16_t crc16(const char* data, std::size_t length) {
    std::uint16_t crc = 0;
    for (std::size_t index = 0; index < length; ++index) {
        const auto byte = static_cast<unsigned char>(data[index]);
        const auto table_index = static_cast<std::uint8_t>((crc >> 8) ^ byte);
        crc = static_cast<std::uint16_t>((crc << 8) ^ detail::kCrc16Table[table_index]);
    }
    return crc;
}

constexpr std::uint16_t crc16(std::string_view data) {
    return crc16(data.data(), data.size());
}

} // namespace cluster
