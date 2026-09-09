#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace persistence {

// IEEE CRC-32 of WAL records, hard-state files and snapshot manifests.
std::uint32_t crc32Ieee(const std::uint8_t* data, std::size_t length);
std::uint32_t crc32Ieee(std::string_view bytes);

// Redis RDB CRC-64 (poly 0xad93d23594c935a9, reflected). WAL does not use this.
std::uint64_t crc64Redis(const std::uint8_t* data, std::size_t length);
std::uint64_t crc64Redis(const std::vector<std::uint8_t>& bytes);

}  // namespace persistence
