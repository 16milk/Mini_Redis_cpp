#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace persistence {

constexpr std::size_t kMaxCodecStringBytes = 64U << 20;
constexpr std::uint32_t kMaxCodecItems = 1U << 22;

class ByteWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(static_cast<char>(value)); }

    void u16(std::uint16_t value) { appendUnsigned(value, 2); }
    void u32(std::uint32_t value) { appendUnsigned(value, 4); }
    void u64(std::uint64_t value) { appendUnsigned(value, 8); }

    void raw(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const char*>(data);
        bytes_.append(bytes, size);
    }

    void raw(std::string_view data) { bytes_.append(data.data(), data.size()); }

    void magic(const char (&value)[4]) { bytes_.append(value, 4); }

    void str(const std::string& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.append(value);
    }

    const std::string& data() const { return bytes_; }
    std::string finish() { return std::move(bytes_); }
    std::size_t size() const { return bytes_.size(); }

private:
    void appendUnsigned(std::uint64_t value, unsigned width) {
        for (unsigned shift = width * 8; shift != 0; shift -= 8) {
            bytes_.push_back(static_cast<char>((value >> (shift - 8)) & 0xffU));
        }
    }

    std::string bytes_;
};

class ByteReader {
public:
    explicit ByteReader(std::string_view bytes) : bytes_(bytes) {}

    bool u8(std::uint8_t& value) {
        std::uint64_t wide = 0;
        if (!readUnsigned(1, wide)) {
            return false;
        }
        value = static_cast<std::uint8_t>(wide);
        return true;
    }

    bool u16(std::uint16_t& value) {
        std::uint64_t wide = 0;
        if (!readUnsigned(2, wide)) {
            return false;
        }
        value = static_cast<std::uint16_t>(wide);
        return true;
    }

    bool u32(std::uint32_t& value) {
        std::uint64_t wide = 0;
        if (!readUnsigned(4, wide)) {
            return false;
        }
        value = static_cast<std::uint32_t>(wide);
        return true;
    }

    bool u64(std::uint64_t& value) { return readUnsigned(8, value); }

    bool magic(const char (&expected)[4]) {
        if (remaining() < 4) {
            return false;
        }
        const bool matches = bytes_.compare(offset_, 4, expected, 4) == 0;
        offset_ += 4;
        return matches;
    }

    bool str(std::string& value, std::size_t max_size = kMaxCodecStringBytes) {
        std::uint32_t size = 0;
        if (!u32(size) || size > max_size || size > remaining()) {
            return false;
        }
        value.assign(bytes_.data() + offset_, size);
        offset_ += size;
        return true;
    }

    bool skip(std::size_t size) {
        if (size > remaining()) {
            return false;
        }
        offset_ += size;
        return true;
    }

    std::string_view view(std::size_t size) {
        if (size > remaining()) {
            return {};
        }
        const std::string_view out(bytes_.data() + offset_, size);
        offset_ += size;
        return out;
    }

    std::size_t remaining() const {
        return offset_ > bytes_.size() ? 0 : bytes_.size() - offset_;
    }

    std::size_t offset() const { return offset_; }
    bool done() const { return offset_ == bytes_.size(); }

private:
    bool readUnsigned(unsigned width, std::uint64_t& value) {
        if (width > remaining()) {
            return false;
        }
        value = 0;
        for (unsigned index = 0; index < width; ++index) {
            value = (value << 8) | static_cast<unsigned char>(bytes_[offset_++]);
        }
        return true;
    }

    std::string_view bytes_;
    std::size_t offset_ = 0;
};

}  // namespace persistence
