#include "mini_redis/persistence/Checksum.hpp"

#include <mutex>

namespace persistence {
namespace {

constexpr std::uint64_t kCrc64Poly = UINT64_C(0xad93d23594c935a9);
constexpr std::uint32_t kCrc32Poly = 0xEDB88320u;

const std::uint32_t* crc32Table() {
    static std::uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t crc = index;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1u) != 0 ? (crc >> 1) ^ kCrc32Poly : crc >> 1;
            }
            table[index] = crc;
        }
    });
    return table;
}

}  // namespace

std::uint32_t crc32Ieee(const std::uint8_t* data, std::size_t length) {
    const std::uint32_t* table = crc32Table();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t offset = 0; offset < length; ++offset) {
        crc = table[(crc ^ data[offset]) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32Ieee(std::string_view bytes) {
    return crc32Ieee(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

std::uint64_t crc64Redis(const std::uint8_t* data, std::size_t length) {
    std::uint64_t crc = 0;
    for (std::size_t offset = 0; offset < length; ++offset) {
        const std::uint8_t byte = data[offset];
        for (std::uint8_t bit_mask = 1; bit_mask != 0; bit_mask <<= 1) {
            const bool top_bit = (crc & (UINT64_C(1) << 63)) != 0;
            const bool input_bit = (byte & bit_mask) != 0;
            crc <<= 1;
            if (top_bit != input_bit) {
                crc ^= kCrc64Poly;
            }
        }
    }

    std::uint64_t reflected = 0;
    for (std::size_t index = 0; index < 64; ++index) {
        reflected = (reflected << 1) | (crc & 1);
        crc >>= 1;
    }
    return reflected;
}

std::uint64_t crc64Redis(const std::vector<std::uint8_t>& bytes) {
    return crc64Redis(bytes.data(), bytes.size());
}

}  // namespace persistence
