#include "mini_redis/persistence/Rdb.hpp"

#include "mini_redis/objects/HashObject.hpp"
#include "mini_redis/objects/ListObject.hpp"
#include "mini_redis/objects/SetObject.hpp"
#include "mini_redis/objects/StringObject.hpp"
#include "mini_redis/objects/ZSetObject.hpp"

#include <array>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr uint8_t RDB_TYPE_STRING = 0;
constexpr uint8_t RDB_TYPE_LIST = 1;
constexpr uint8_t RDB_TYPE_SET = 2;
constexpr uint8_t RDB_TYPE_ZSET = 3;
constexpr uint8_t RDB_TYPE_HASH = 4;
constexpr uint8_t RDB_TYPE_ZSET_2 = 5;
constexpr uint8_t RDB_TYPE_LIST_ZIPLIST = 10;
constexpr uint8_t RDB_TYPE_SET_INTSET = 11;
constexpr uint8_t RDB_TYPE_ZSET_ZIPLIST = 12;
constexpr uint8_t RDB_TYPE_HASH_ZIPLIST = 13;
constexpr uint8_t RDB_TYPE_LIST_QUICKLIST = 14;
constexpr uint8_t RDB_TYPE_HASH_LISTPACK = 16;
constexpr uint8_t RDB_TYPE_ZSET_LISTPACK = 17;
constexpr uint8_t RDB_TYPE_LIST_QUICKLIST_2 = 18;
constexpr uint8_t RDB_TYPE_SET_LISTPACK = 20;

constexpr uint8_t RDB_OPCODE_IDLE = 248;
constexpr uint8_t RDB_OPCODE_FREQ = 249;
constexpr uint8_t RDB_OPCODE_AUX = 250;
constexpr uint8_t RDB_OPCODE_RESIZEDB = 251;
constexpr uint8_t RDB_OPCODE_EXPIRETIME_MS = 252;
constexpr uint8_t RDB_OPCODE_EXPIRETIME = 253;
constexpr uint8_t RDB_OPCODE_SELECTDB = 254;
constexpr uint8_t RDB_OPCODE_EOF = 255;

constexpr uint8_t RDB_ENCVAL = 3;
constexpr uint8_t RDB_ENC_INT8 = 0;
constexpr uint8_t RDB_ENC_INT16 = 1;
constexpr uint8_t RDB_ENC_INT32 = 2;
constexpr uint8_t RDB_ENC_LZF = 3;

constexpr uint64_t CRC64_POLY = UINT64_C(0xad93d23594c935a9);
constexpr uint32_t LISTPACK_HEADER_SIZE = 6;
constexpr uint8_t LISTPACK_EOF = 0xff;
constexpr size_t MAX_RDB_STRING_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr uint64_t MAX_RDB_COLLECTION_ENTRIES = 100ULL * 1000ULL * 1000ULL;
constexpr size_t MAX_RDB_FILE_BYTES = 1024ULL * 1024ULL * 1024ULL;

thread_local std::string g_last_error;

std::string errnoMessage(const std::string& operation, const std::string& path) {
    return operation + " '" + path + "': " + std::strerror(errno);
}

void logError(const std::string& message) {
    g_last_error = message;
    std::fprintf(stderr, "[RDB] %s\n", message.c_str());
}

uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t readLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t readLe64(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(data[index]) << (index * 8);
    }
    return value;
}

uint32_t readBe32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint64_t readBe64(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index) {
        value = (value << 8) | data[index];
    }
    return value;
}

void appendLe64(std::vector<uint8_t>& output, uint64_t value) {
    for (size_t index = 0; index < 8; ++index) {
        output.push_back(static_cast<uint8_t>(value >> (index * 8)));
    }
}

UnixMillis currentUnixMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

UnixMillis checkedExpireMilliseconds(uint64_t value) {
    if (value > static_cast<uint64_t>(std::numeric_limits<UnixMillis>::max())) {
        throw std::overflow_error("RDB millisecond expiration exceeds UnixMillis range");
    }
    return static_cast<UnixMillis>(value);
}

UnixMillis checkedExpireSeconds(uint32_t value) {
    constexpr uint64_t MILLIS_PER_SECOND = 1000;
    if (static_cast<uint64_t>(value) >
        static_cast<uint64_t>(std::numeric_limits<UnixMillis>::max()) /
            MILLIS_PER_SECOND) {
        throw std::overflow_error("RDB second expiration overflows UnixMillis");
    }
    return static_cast<UnixMillis>(value) *
           static_cast<UnixMillis>(MILLIS_PER_SECOND);
}

uint64_t crc64(const uint8_t* data, size_t length) {
    uint64_t crc = 0;
    for (size_t offset = 0; offset < length; ++offset) {
        const uint8_t byte = data[offset];
        for (uint8_t bit_mask = 1; bit_mask != 0; bit_mask <<= 1) {
            const bool top_bit = (crc & (UINT64_C(1) << 63)) != 0;
            const bool input_bit = (byte & bit_mask) != 0;
            crc <<= 1;
            if (top_bit != input_bit) {
                crc ^= CRC64_POLY;
            }
        }
    }

    uint64_t reflected = 0;
    for (size_t index = 0; index < 64; ++index) {
        reflected = (reflected << 1) | (crc & 1);
        crc >>= 1;
    }
    return reflected;
}

class BufferWriter {
public:
    void writeByte(uint8_t value) { bytes_.push_back(value); }

    void writeRaw(const void* data, size_t length) {
        const auto* first = static_cast<const uint8_t*>(data);
        bytes_.insert(bytes_.end(), first, first + length);
    }

    void writeLen(uint64_t length) {
        if (length < (UINT64_C(1) << 6)) {
            writeByte(static_cast<uint8_t>(length));
        } else if (length < (UINT64_C(1) << 14)) {
            writeByte(static_cast<uint8_t>(0x40 | (length >> 8)));
            writeByte(static_cast<uint8_t>(length));
        } else if (length <= std::numeric_limits<uint32_t>::max()) {
            writeByte(0x80);
            for (int shift = 24; shift >= 0; shift -= 8) {
                writeByte(static_cast<uint8_t>(length >> shift));
            }
        } else {
            writeByte(0x81);
            for (int shift = 56; shift >= 0; shift -= 8) {
                writeByte(static_cast<uint8_t>(length >> shift));
            }
        }
    }

    void writeString(const std::string& value) {
        writeLen(value.size());
        writeRaw(value.data(), value.size());
    }

    void writeBinaryDouble(double value) {
        static_assert(sizeof(double) == sizeof(uint64_t), "RDB requires IEEE754 binary64");
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendLe64(bytes_, bits);
    }

    const std::vector<uint8_t>& bytes() const { return bytes_; }
    std::vector<uint8_t>& bytes() { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

class BufferReader {
public:
    explicit BufferReader(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

    uint8_t readByte() {
        require(1);
        return bytes_[position_++];
    }

    std::vector<uint8_t> readRaw(size_t length) {
        require(length);
        std::vector<uint8_t> result(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                                    bytes_.begin() + static_cast<std::ptrdiff_t>(position_ + length));
        position_ += length;
        return result;
    }

    void skip(size_t length) {
        require(length);
        position_ += length;
    }

    uint64_t readLen(bool* encoded = nullptr) {
        if (encoded) {
            *encoded = false;
        }

        const uint8_t first = readByte();
        const uint8_t type = first >> 6;
        if (type == 0) {
            return first & 0x3f;
        }
        if (type == 1) {
            return (static_cast<uint64_t>(first & 0x3f) << 8) | readByte();
        }
        if (first == 0x80) {
            const auto raw = readRaw(4);
            return readBe32(raw.data());
        }
        if (first == 0x81) {
            const auto raw = readRaw(8);
            return readBe64(raw.data());
        }
        if (type == RDB_ENCVAL) {
            if (encoded) {
                *encoded = true;
            }
            return first & 0x3f;
        }
        throw std::runtime_error("unknown RDB length encoding");
    }

    uint64_t readPlainLen() {
        bool encoded = false;
        const uint64_t length = readLen(&encoded);
        if (encoded) {
            throw std::runtime_error("encoded RDB length used where a plain length is required");
        }
        return length;
    }

    std::string readString() {
        bool encoded = false;
        const uint64_t length_or_encoding = readLen(&encoded);
        if (!encoded) {
            if (length_or_encoding > MAX_RDB_STRING_BYTES ||
                length_or_encoding > remaining()) {
                throw std::runtime_error("RDB string length exceeds available data");
            }
            const auto raw = readRaw(static_cast<size_t>(length_or_encoding));
            return {reinterpret_cast<const char*>(raw.data()), raw.size()};
        }

        int64_t value = 0;
        switch (length_or_encoding) {
        case RDB_ENC_INT8:
            value = static_cast<int8_t>(readByte());
            break;
        case RDB_ENC_INT16: {
            const auto raw = readRaw(2);
            value = static_cast<int16_t>(readLe16(raw.data()));
            break;
        }
        case RDB_ENC_INT32: {
            const auto raw = readRaw(4);
            value = static_cast<int32_t>(readLe32(raw.data()));
            break;
        }
        case RDB_ENC_LZF:
            return readLzfString();
        default:
            throw std::runtime_error("unsupported RDB string encoding");
        }
        return std::to_string(value);
    }

    double readBinaryDouble() {
        const auto raw = readRaw(8);
        const uint64_t bits = readLe64(raw.data());
        double value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        if (std::isnan(value)) {
            throw std::runtime_error("RDB ZSET score is NaN");
        }
        return value;
    }

    double readLegacyDouble() {
        const uint8_t length = readByte();
        if (length == 255) {
            throw std::runtime_error("NaN ZSET scores are not supported");
        }
        if (length == 254) return std::numeric_limits<double>::infinity();
        if (length == 253) return -std::numeric_limits<double>::infinity();
        const auto raw = readRaw(length);
        const std::string score_text(reinterpret_cast<const char*>(raw.data()), raw.size());
        char* end = nullptr;
        errno = 0;
        const double score = std::strtod(score_text.c_str(), &end);
        if (errno == ERANGE || end != score_text.c_str() + score_text.size() ||
            !std::isfinite(score)) {
            throw std::runtime_error("invalid legacy ZSET score in RDB");
        }
        return score;
    }

    size_t position() const { return position_; }
    size_t size() const { return bytes_.size(); }
    size_t remaining() const { return bytes_.size() - position_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    void require(size_t length) const {
        if (length > remaining()) {
            throw std::runtime_error("unexpected end of RDB file");
        }
    }

    std::string readLzfString() {
        bool encoded = false;
        const uint64_t compressed_length = readLen(&encoded);
        if (encoded) {
            throw std::runtime_error("invalid LZF compressed length");
        }
        const uint64_t uncompressed_length = readLen(&encoded);
        if (encoded || uncompressed_length > MAX_RDB_STRING_BYTES ||
            compressed_length > remaining()) {
            throw std::runtime_error("invalid LZF string lengths");
        }

        const auto input = readRaw(static_cast<size_t>(compressed_length));
        std::string output(static_cast<size_t>(uncompressed_length), '\0');
        size_t input_position = 0;
        size_t output_position = 0;
        while (input_position < input.size()) {
            const uint8_t control = input[input_position++];
            if (control < 32) {
                const size_t length = static_cast<size_t>(control) + 1;
                if (input_position + length > input.size() || output_position + length > output.size()) {
                    throw std::runtime_error("invalid LZF literal run");
                }
                std::memcpy(output.data() + output_position, input.data() + input_position, length);
                input_position += length;
                output_position += length;
            } else {
                size_t length = control >> 5;
                if (input_position >= input.size()) {
                    throw std::runtime_error("invalid LZF back reference");
                }
                size_t reference = (static_cast<size_t>(control & 0x1f) << 8) + input[input_position++];
                if (length == 7) {
                    if (input_position >= input.size()) {
                        throw std::runtime_error("invalid LZF extended run");
                    }
                    length += input[input_position++];
                }
                length += 2;
                reference += 1;
                if (reference > output_position || output_position + length > output.size()) {
                    throw std::runtime_error("invalid LZF back reference range");
                }
                for (size_t index = 0; index < length; ++index) {
                    output[output_position] = output[output_position - reference];
                    ++output_position;
                }
            }
        }
        if (output_position != output.size()) {
            throw std::runtime_error("invalid LZF output length");
        }
        return output;
    }

    std::vector<uint8_t> bytes_;
    size_t position_ = 0;
};

void ensureCollectionLength(uint64_t length, const char* collection_type) {
    if (length > MAX_RDB_COLLECTION_ENTRIES) {
        throw std::runtime_error(std::string("RDB ") + collection_type +
                                 " entry count is too large");
    }
}

void writeObject(BufferWriter& writer, const std::string& key, const RedisObject& object) {
    switch (object.type()) {
    case ObjectType::STRING: {
        writer.writeByte(RDB_TYPE_STRING);
        writer.writeString(key);
        writer.writeString(static_cast<const StringObject&>(object).value());
        return;
    }
    case ObjectType::LIST: {
        writer.writeByte(RDB_TYPE_LIST);
        writer.writeString(key);
        const auto values = static_cast<const ListObject&>(object).values();
        writer.writeLen(values.size());
        for (const auto& value : values) {
            writer.writeString(value);
        }
        return;
    }
    case ObjectType::SET: {
        writer.writeByte(RDB_TYPE_SET);
        writer.writeString(key);
        const auto members = static_cast<const SetObject&>(object).members();
        writer.writeLen(members.size());
        for (const auto& member : members) {
            writer.writeString(member);
        }
        return;
    }
    case ObjectType::ZSET: {
        writer.writeByte(RDB_TYPE_ZSET_2);
        writer.writeString(key);
        const auto entries = static_cast<const ZSetObject&>(object).members_with_scores();
        writer.writeLen(entries.size());
        for (const auto& [member, score] : entries) {
            writer.writeString(member);
            writer.writeBinaryDouble(score);
        }
        return;
    }
    case ObjectType::HASH: {
        writer.writeByte(RDB_TYPE_HASH);
        writer.writeString(key);
        const auto fields = static_cast<const HashObject&>(object).get_all_fields();
        writer.writeLen(fields.size());
        for (const auto& [field, value] : fields) {
            writer.writeString(field);
            writer.writeString(value);
        }
        return;
    }
    }

    throw std::runtime_error("unsupported Redis object type while saving RDB");
}

std::shared_ptr<RedisObject> readTraditionalObject(
    BufferReader& reader, uint8_t type, const std::string& key) {
    (void)key;
    if (type == RDB_TYPE_STRING) {
        return std::make_shared<StringObject>(reader.readString());
    }

    const uint64_t count = reader.readPlainLen();
    ensureCollectionLength(count, "collection");

    if (type == RDB_TYPE_LIST) {
        auto object = std::make_shared<ListObject>();
        for (uint64_t index = 0; index < count; ++index) {
            object->push_back(reader.readString());
        }
        return object;
    }
    if (type == RDB_TYPE_SET) {
        auto object = std::make_shared<SetObject>();
        for (uint64_t index = 0; index < count; ++index) {
            object->add(reader.readString());
        }
        return object;
    }
    if (type == RDB_TYPE_HASH) {
        auto object = std::make_shared<HashObject>();
        for (uint64_t index = 0; index < count; ++index) {
            object->set_field(reader.readString(), reader.readString());
        }
        return object;
    }
    if (type == RDB_TYPE_ZSET) {
        auto object = std::make_shared<ZSetObject>();
        for (uint64_t index = 0; index < count; ++index) {
            const std::string member = reader.readString();
            object->add(reader.readLegacyDouble(), member);
        }
        return object;
    }
    if (type == RDB_TYPE_ZSET_2) {
        auto object = std::make_shared<ZSetObject>();
        for (uint64_t index = 0; index < count; ++index) {
            const std::string member = reader.readString();
            object->add(reader.readBinaryDouble(), member);
        }
        return object;
    }

    throw std::runtime_error("unsupported traditional RDB object type " + std::to_string(type));
}

int64_t decodeSignedLittleEndian(const uint8_t* data, size_t width) {
    uint64_t value = 0;
    for (size_t index = 0; index < width; ++index) {
        value |= static_cast<uint64_t>(data[index]) << (8 * index);
    }
    if (width == 8) {
        if ((value & (UINT64_C(1) << 63)) == 0) {
            return static_cast<int64_t>(value);
        }
        return std::numeric_limits<int64_t>::min() +
               static_cast<int64_t>(value & std::numeric_limits<int64_t>::max());
    }

    const uint64_t sign_bit = UINT64_C(1) << (width * 8 - 1);
    if (value & sign_bit) {
        value |= ~((UINT64_C(1) << (width * 8)) - 1);
    }
    return static_cast<int64_t>(value);
}

std::vector<int64_t> decodeIntset(const std::string& blob) {
    if (blob.size() < 8) {
        throw std::runtime_error("invalid intset payload");
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(blob.data());
    const uint32_t encoding = readLe32(bytes);
    const uint32_t count = readLe32(bytes + 4);
    if (encoding != 2 && encoding != 4 && encoding != 8) {
        throw std::runtime_error("invalid intset encoding");
    }
    const uint64_t expected = 8ULL + static_cast<uint64_t>(encoding) * count;
    if (expected != blob.size()) {
        throw std::runtime_error("invalid intset payload length");
    }
    ensureCollectionLength(count, "intset");

    std::vector<int64_t> values;
    values.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        values.push_back(decodeSignedLittleEndian(bytes + 8 + static_cast<size_t>(index) * encoding,
                                                  encoding));
    }
    return values;
}

std::vector<std::string> decodeZiplist(const std::string& blob) {
    const std::vector<uint8_t> bytes(blob.begin(), blob.end());
    if (bytes.size() < 11 || readLe32(bytes.data()) != bytes.size() || bytes.back() != 0xff) {
        throw std::runtime_error("invalid ziplist payload");
    }

    const uint32_t declared_tail = readLe32(bytes.data() + 4);
    const uint16_t declared_count = readLe16(bytes.data() + 8);
    std::vector<std::string> values;
    size_t position = 10;
    size_t previous_entry_length = 0;
    size_t last_entry_start = 10;

    while (position < bytes.size() - 1) {
        const size_t entry_start = position;
        const uint8_t previous_length_marker = bytes[position++];
        size_t stored_previous_length = previous_length_marker;
        if (previous_length_marker == 254) {
            if (position + 4 > bytes.size() - 1) {
                throw std::runtime_error("truncated ziplist previous length");
            }
            stored_previous_length = readLe32(bytes.data() + position);
            position += 4;
        }
        if (!values.empty() && stored_previous_length != previous_entry_length) {
            throw std::runtime_error("invalid ziplist previous entry length");
        }

        if (position >= bytes.size() - 1) {
            throw std::runtime_error("truncated ziplist entry");
        }
        const uint8_t encoding = bytes[position++];
        std::string value;
        size_t string_length = 0;

        if ((encoding & 0xc0) == 0x00) {
            string_length = encoding & 0x3f;
        } else if ((encoding & 0xc0) == 0x40) {
            if (position >= bytes.size() - 1) throw std::runtime_error("truncated ziplist string length");
            string_length = (static_cast<size_t>(encoding & 0x3f) << 8) | bytes[position++];
        } else if (encoding == 0x80) {
            if (position + 4 > bytes.size() - 1) throw std::runtime_error("truncated ziplist string length");
            string_length = readBe32(bytes.data() + position);
            position += 4;
        } else if (encoding == 0xfe || encoding == 0xc0 || encoding == 0xd0 ||
                   encoding == 0xe0 || encoding == 0xf0) {
            const size_t width = encoding == 0xfe ? 1 : encoding == 0xc0 ? 2
                : encoding == 0xd0 ? 4 : encoding == 0xe0 ? 8 : 3;
            if (position + width > bytes.size() - 1) throw std::runtime_error("truncated ziplist integer");
            value = std::to_string(decodeSignedLittleEndian(bytes.data() + position, width));
            position += width;
        } else if (encoding >= 0xf1 && encoding <= 0xfd) {
            value = std::to_string((encoding & 0x0f) - 1);
        } else {
            throw std::runtime_error("unsupported ziplist entry encoding");
        }

        if (string_length != 0) {
            if (string_length > bytes.size() - position - 1) {
                throw std::runtime_error("truncated ziplist string");
            }
            value.assign(reinterpret_cast<const char*>(bytes.data() + position), string_length);
            position += string_length;
        }

        previous_entry_length = position - entry_start;
        last_entry_start = entry_start;
        values.push_back(std::move(value));
        if (values.size() > MAX_RDB_COLLECTION_ENTRIES) {
            throw std::runtime_error("ziplist entry count is too large");
        }
    }

    if (position != bytes.size() - 1 ||
        (!values.empty() && declared_tail != last_entry_start) ||
        (values.empty() && declared_tail != 10) ||
        (declared_count != 65535 && declared_count != values.size())) {
        throw std::runtime_error("invalid ziplist header");
    }
    return values;
}

size_t decodeListpackBacklen(const std::vector<uint8_t>& bytes, size_t position, size_t entry_length) {
    const size_t encoded_length = [&]() {
        if (entry_length <= 127) return size_t{1};
        if (entry_length < 16383) return size_t{2};
        if (entry_length < 2097151) return size_t{3};
        if (entry_length < 268435455) return size_t{4};
        return size_t{5};
    }();
    if (position + encoded_length > bytes.size()) {
        throw std::runtime_error("truncated listpack backlen");
    }

    uint64_t decoded = 0;
    uint64_t shift = 0;
    for (size_t offset = 0; offset < encoded_length; ++offset) {
        const uint8_t byte = bytes[position + encoded_length - 1 - offset];
        decoded |= static_cast<uint64_t>(byte & 0x7f) << shift;
        shift += 7;
    }
    if (decoded != entry_length) {
        throw std::runtime_error("invalid listpack backlen");
    }
    return encoded_length;
}

std::vector<std::string> decodeListpack(const std::string& blob) {
    const std::vector<uint8_t> bytes(blob.begin(), blob.end());
    if (bytes.size() < LISTPACK_HEADER_SIZE + 1 || readLe32(bytes.data()) != bytes.size() ||
        bytes.back() != LISTPACK_EOF) {
        throw std::runtime_error("invalid listpack payload");
    }

    const uint16_t declared_count = readLe16(bytes.data() + 4);
    std::vector<std::string> values;
    size_t position = LISTPACK_HEADER_SIZE;
    while (position < bytes.size() - 1) {
        const size_t entry_start = position;
        const uint8_t first = bytes[position++];
        std::string value;
        size_t data_length = 0;

        if ((first & 0x80) == 0) {
            value = std::to_string(first & 0x7f);
        } else if ((first & 0xc0) == 0x80) {
            data_length = first & 0x3f;
        } else if ((first & 0xe0) == 0xc0) {
            if (position >= bytes.size()) throw std::runtime_error("truncated listpack integer");
            uint64_t raw = (static_cast<uint64_t>(first & 0x1f) << 8) | bytes[position++];
            const int64_t signed_value = raw >= (UINT64_C(1) << 12)
                                             ? static_cast<int64_t>(raw) - (UINT64_C(1) << 13)
                                             : static_cast<int64_t>(raw);
            value = std::to_string(signed_value);
        } else if ((first & 0xf0) == 0xe0) {
            if (position >= bytes.size()) throw std::runtime_error("truncated listpack string length");
            data_length = (static_cast<size_t>(first & 0x0f) << 8) | bytes[position++];
        } else if (first == 0xf0) {
            if (position + 4 > bytes.size()) throw std::runtime_error("truncated listpack string length");
            data_length = readLe32(bytes.data() + position);
            position += 4;
        } else if (first == 0xf1 || first == 0xf2 || first == 0xf3 || first == 0xf4) {
            const size_t width = first == 0xf1 ? 2 : first == 0xf2 ? 3 : first == 0xf3 ? 4 : 8;
            if (position + width > bytes.size()) throw std::runtime_error("truncated listpack integer");
            value = std::to_string(decodeSignedLittleEndian(bytes.data() + position, width));
            position += width;
        } else {
            throw std::runtime_error("unsupported listpack entry encoding");
        }

        if (data_length != 0) {
            if (data_length > bytes.size() - position - 1) {
                throw std::runtime_error("truncated listpack string");
            }
            value.assign(reinterpret_cast<const char*>(bytes.data() + position), data_length);
            position += data_length;
        }

        const size_t entry_length = position - entry_start;
        position += decodeListpackBacklen(bytes, position, entry_length);
        values.push_back(std::move(value));
        if (values.size() > MAX_RDB_COLLECTION_ENTRIES) {
            throw std::runtime_error("listpack entry count is too large");
        }
    }
    if (declared_count != UINT16_MAX && declared_count != values.size()) {
        throw std::runtime_error("invalid listpack header count");
    }
    return values;
}

void loadEncodedObject(BufferReader& reader, uint8_t type,
                       const std::string& key,
                       std::unordered_map<std::string, std::shared_ptr<RedisObject>>& data) {
    const std::string blob = reader.readString();
    if (type == RDB_TYPE_SET_INTSET) {
        auto object = std::make_shared<SetObject>();
        for (const int64_t value : decodeIntset(blob)) {
            object->add(std::to_string(value));
        }
        data[key] = std::move(object);
        return;
    }

    const bool ziplist = type == RDB_TYPE_HASH_ZIPLIST || type == RDB_TYPE_ZSET_ZIPLIST ||
                         type == RDB_TYPE_LIST_ZIPLIST;
    const auto values = ziplist ? decodeZiplist(blob) : decodeListpack(blob);
    if (type == RDB_TYPE_LIST_ZIPLIST) {
        auto object = std::make_shared<ListObject>();
        for (const auto& value : values) {
            object->push_back(value);
        }
        data[key] = std::move(object);
        return;
    }
    if (type == RDB_TYPE_HASH_ZIPLIST) {
        if (values.size() % 2 != 0) throw std::runtime_error("odd hash ziplist element count");
        auto object = std::make_shared<HashObject>();
        for (size_t index = 0; index < values.size(); index += 2) {
            object->set_field(values[index], values[index + 1]);
        }
        data[key] = std::move(object);
        return;
    }
    if (type == RDB_TYPE_ZSET_ZIPLIST) {
        if (values.size() % 2 != 0) throw std::runtime_error("odd ZSET ziplist element count");
        auto object = std::make_shared<ZSetObject>();
        for (size_t index = 0; index < values.size(); index += 2) {
            char* end = nullptr;
            errno = 0;
            const double score = std::strtod(values[index + 1].c_str(), &end);
            if (errno == ERANGE || end != values[index + 1].c_str() + values[index + 1].size() ||
                std::isnan(score)) {
                throw std::runtime_error("invalid ZSET ziplist score");
            }
            object->add(score, values[index]);
        }
        data[key] = std::move(object);
        return;
    }
    if (type == RDB_TYPE_HASH_LISTPACK) {
        if (values.size() % 2 != 0) throw std::runtime_error("odd hash listpack element count");
        auto object = std::make_shared<HashObject>();
        for (size_t index = 0; index < values.size(); index += 2) {
            object->set_field(values[index], values[index + 1]);
        }
        data[key] = std::move(object);
        return;
    }
    if (type == RDB_TYPE_ZSET_LISTPACK) {
        if (values.size() % 2 != 0) throw std::runtime_error("odd ZSET listpack element count");
        auto object = std::make_shared<ZSetObject>();
        for (size_t index = 0; index < values.size(); index += 2) {
            char* end = nullptr;
            errno = 0;
            const double score = std::strtod(values[index + 1].c_str(), &end);
            if (errno == ERANGE || end != values[index + 1].c_str() + values[index + 1].size() ||
                std::isnan(score)) {
                throw std::runtime_error("invalid ZSET listpack score");
            }
            object->add(score, values[index]);
        }
        data[key] = std::move(object);
        return;
    }
    if (type == RDB_TYPE_SET_LISTPACK) {
        auto object = std::make_shared<SetObject>();
        for (const auto& value : values) {
            object->add(value);
        }
        data[key] = std::move(object);
        return;
    }

    throw std::runtime_error("unsupported encoded RDB object type " + std::to_string(type));
}

std::shared_ptr<RedisObject> readQuicklist(BufferReader& reader, uint8_t type) {
    const uint64_t node_count = reader.readPlainLen();
    ensureCollectionLength(node_count, "quicklist node");
    auto object = std::make_shared<ListObject>();
    for (uint64_t index = 0; index < node_count; ++index) {
        const uint64_t container = type == RDB_TYPE_LIST_QUICKLIST_2
            ? reader.readPlainLen() : 2;
        const std::string payload = reader.readString();
        if (container == 1) {
            object->push_back(payload);
        } else if (container == 2) {
            const auto values = type == RDB_TYPE_LIST_QUICKLIST_2
                ? decodeListpack(payload) : decodeZiplist(payload);
            for (const auto& value : values) {
                object->push_back(value);
            }
        } else {
            throw std::runtime_error("unsupported quicklist node container");
        }
    }
    return object;
}

std::vector<uint8_t> readFile(const std::string& filename) {
    std::ifstream input(filename, std::ios::binary);
    if (!input) {
        throw std::runtime_error(errnoMessage("cannot open RDB file", filename));
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > MAX_RDB_FILE_BYTES) {
        throw std::runtime_error("cannot determine RDB file size");
    }
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("cannot read complete RDB file");
    }
    return bytes;
}

bool writeAtomically(const std::string& filename, const std::vector<uint8_t>& bytes) {
    std::string temporary_template = filename + ".tmp.XXXXXX";
    std::vector<char> temporary_path(temporary_template.begin(), temporary_template.end());
    temporary_path.push_back('\0');
    int fd = mkstemp(temporary_path.data());
    const std::string temporary(temporary_path.data());
    if (fd == -1) {
        logError(errnoMessage("cannot create temporary RDB file", filename));
        return false;
    }
    if (fchmod(fd, 0644) == -1) {
        logError(errnoMessage("cannot set permissions on temporary RDB file", temporary));
        close(fd);
        std::remove(temporary.c_str());
        return false;
    }

    bool success = true;
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written > 0) {
            offset += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        logError(errnoMessage("cannot write temporary RDB file", temporary));
        success = false;
        break;
    }

    if (success && fsync(fd) == -1) {
        logError(errnoMessage("cannot fsync temporary RDB file", temporary));
        success = false;
    }
    if (close(fd) == -1 && success) {
        logError(errnoMessage("cannot close temporary RDB file", temporary));
        success = false;
    }
    if (!success) {
        std::remove(temporary.c_str());
        return false;
    }

    if (rename(temporary.c_str(), filename.c_str()) == -1) {
        logError(errnoMessage("cannot atomically replace RDB file", filename));
        std::remove(temporary.c_str());
        return false;
    }

    const size_t slash = filename.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "."
        : slash == 0 ? "/" : filename.substr(0, slash);
    const int directory_fd = open(directory.c_str(), O_RDONLY);
    if (directory_fd == -1) {
        logError(errnoMessage("RDB file replaced but cannot open parent directory for fsync", directory));
        return false;
    }
    if (fsync(directory_fd) == -1) {
        logError(errnoMessage("RDB file replaced but cannot fsync parent directory", directory));
        close(directory_fd);
        return false;
    }
    close(directory_fd);

    return true;
}

} // namespace

const std::string& RdbEncoder::lastError() {
    return g_last_error;
}

bool RdbEncoder::saveToFile(
    const std::string& filename,
    const ObjectMap& objects,
    const ExpireMap& expires,
    UnixMillis snapshot_now_ms) {
    g_last_error.clear();
    try {
        for (const auto& [key, deadline_ms] : expires) {
            (void)deadline_ms;
            if (objects.count(key) == 0) {
                throw std::runtime_error(
                    "expiration metadata references missing key '" + key + "'");
            }
        }

        size_t live_object_count = 0;
        size_t live_expire_count = 0;
        for (const auto& [key, object] : objects) {
            if (!object) {
                throw std::runtime_error("cannot save null object for key '" + key + "'");
            }
            const auto expire_it = expires.find(key);
            if (expire_it != expires.end() && expire_it->second < 0) {
                throw std::runtime_error(
                    "cannot encode negative expiration for key '" + key + "'");
            }
            if (expire_it != expires.end() && expire_it->second <= snapshot_now_ms) {
                continue;
            }
            ++live_object_count;
            if (expire_it != expires.end()) {
                ++live_expire_count;
            }
        }

        BufferWriter writer;
        writer.writeRaw("REDIS0009", 9);
        writer.writeByte(RDB_OPCODE_AUX);
        writer.writeString("redis-ver");
        writer.writeString("mini_redis_cpp");
        writer.writeByte(RDB_OPCODE_SELECTDB);
        writer.writeLen(0);
        writer.writeByte(RDB_OPCODE_RESIZEDB);
        writer.writeLen(live_object_count);
        writer.writeLen(live_expire_count);

        for (const auto& [key, object] : objects) {
            const auto expire_it = expires.find(key);
            if (expire_it != expires.end() && expire_it->second <= snapshot_now_ms) {
                continue;
            }
            if (expire_it != expires.end()) {
                writer.writeByte(RDB_OPCODE_EXPIRETIME_MS);
                appendLe64(writer.bytes(), static_cast<uint64_t>(expire_it->second));
            }
            writeObject(writer, key, *object);
        }

        writer.writeByte(RDB_OPCODE_EOF);
        const uint64_t checksum = crc64(writer.bytes().data(), writer.bytes().size());
        appendLe64(writer.bytes(), checksum);
        return writeAtomically(filename, writer.bytes());
    } catch (const std::exception& exception) {
        logError(std::string("cannot save RDB '") + filename + "': " + exception.what());
        return false;
    }
}

bool RdbEncoder::saveToFile(const std::string& filename, const ObjectMap& objects) {
    return saveToFile(filename, objects, {}, currentUnixMillis());
}

RdbLoadResult RdbEncoder::loadFromFile(
    const std::string& filename,
    UnixMillis load_now_ms) {
    g_last_error.clear();
    if (access(filename.c_str(), F_OK) == -1 && errno == ENOENT) {
        return {};
    }
    try {
        return RdbDecoder(filename).decodeAll(load_now_ms);
    } catch (const std::exception& exception) {
        logError(std::string("cannot load RDB '") + filename + "': " + exception.what());
        return {};
    }
}

ObjectMap RdbEncoder::loadFromFile(const std::string& filename) {
    return loadFromFile(filename, currentUnixMillis()).objects;
}

RdbDecoder::RdbDecoder(std::string filename) : filename_(std::move(filename)) {}

RdbLoadResult RdbDecoder::decodeAll(UnixMillis load_now_ms) {
    BufferReader reader(readFile(filename_));
    if (reader.size() < 10) {
        throw std::runtime_error("RDB file is too short");
    }

    const auto magic = reader.readRaw(9);
    const std::string header(reinterpret_cast<const char*>(magic.data()), magic.size());
    if (header.compare(0, 5, "REDIS") != 0 ||
        header[5] < '0' || header[5] > '9' || header[6] < '0' || header[6] > '9' ||
        header[7] < '0' || header[7] > '9' || header[8] < '0' || header[8] > '9') {
        throw std::runtime_error("invalid RDB magic or version");
    }
    const int version = std::stoi(header.substr(5));
    if (version < 1 || version > 15) {
        throw std::runtime_error("unsupported RDB version " + std::to_string(version));
    }

    const auto& all_bytes = reader.bytes();
    const size_t checksum_length = version >= 5 ? 8 : 0;
    if (reader.size() < 10 + checksum_length) {
        throw std::runtime_error("RDB file is too short for its declared version");
    }
    const size_t payload_end = all_bytes.size() - checksum_length;
    if (version >= 5) {
        const uint64_t stored_checksum = readLe64(all_bytes.data() + payload_end);
        const uint64_t computed_checksum = crc64(all_bytes.data(), payload_end);
        if (stored_checksum != 0 && stored_checksum != computed_checksum) {
            throw std::runtime_error("RDB CRC64 checksum mismatch");
        }
    }

    RdbLoadResult result;
    std::unordered_set<std::string> seen_keys;
    uint64_t selected_db = 0;
    UnixMillis pending_expire_at_ms = 0;
    bool has_pending_expire = false;
    bool has_pending_idle = false;
    bool has_pending_freq = false;
    bool saw_eof = false;
    while (reader.position() < payload_end) {
        const uint8_t type = reader.readByte();
        if (type == RDB_OPCODE_EOF) {
            if (has_pending_expire || has_pending_idle || has_pending_freq) {
                throw std::runtime_error("RDB key metadata has no following object");
            }
            saw_eof = true;
            break;
        }
        if (type == RDB_OPCODE_AUX) {
            if (has_pending_expire || has_pending_idle || has_pending_freq) {
                throw std::runtime_error(
                    "RDB key metadata must be followed by an object");
            }
            (void)reader.readString();
            (void)reader.readString();
            continue;
        }
        if (type == RDB_OPCODE_SELECTDB) {
            if (has_pending_expire || has_pending_idle || has_pending_freq) {
                throw std::runtime_error(
                    "RDB key metadata must be followed by an object");
            }
            selected_db = reader.readPlainLen();
            continue;
        }
        if (type == RDB_OPCODE_RESIZEDB) {
            if (has_pending_expire || has_pending_idle || has_pending_freq) {
                throw std::runtime_error(
                    "RDB key metadata must be followed by an object");
            }
            (void)reader.readPlainLen();
            (void)reader.readPlainLen();
            continue;
        }
        if (type == RDB_OPCODE_EXPIRETIME) {
            if (has_pending_expire) {
                throw std::runtime_error("duplicate RDB expiration metadata for one object");
            }
            const auto raw = reader.readRaw(4);
            pending_expire_at_ms = checkedExpireSeconds(readLe32(raw.data()));
            has_pending_expire = true;
            continue;
        }
        if (type == RDB_OPCODE_EXPIRETIME_MS) {
            if (has_pending_expire) {
                throw std::runtime_error("duplicate RDB expiration metadata for one object");
            }
            const auto raw = reader.readRaw(8);
            pending_expire_at_ms = checkedExpireMilliseconds(readLe64(raw.data()));
            has_pending_expire = true;
            continue;
        }
        if (type == RDB_OPCODE_IDLE) {
            if (has_pending_idle) {
                throw std::runtime_error("duplicate RDB idle metadata for one object");
            }
            (void)reader.readPlainLen();
            has_pending_idle = true;
            continue;
        }
        if (type == RDB_OPCODE_FREQ) {
            if (has_pending_freq) {
                throw std::runtime_error("duplicate RDB frequency metadata for one object");
            }
            reader.skip(1);
            has_pending_freq = true;
            continue;
        }

        const std::string key = reader.readString();
        if (selected_db != 0) {
            throw std::runtime_error("RDB contains unsupported database " +
                                     std::to_string(selected_db));
        }
        if (!seen_keys.emplace(key).second) {
            throw std::runtime_error("duplicate key in RDB: '" + key + "'");
        }

        std::shared_ptr<RedisObject> object;
        if (type <= RDB_TYPE_ZSET_2) {
            object = readTraditionalObject(reader, type, key);
        } else if (type == RDB_TYPE_SET_INTSET || type == RDB_TYPE_SET_LISTPACK ||
                   type == RDB_TYPE_ZSET_LISTPACK || type == RDB_TYPE_HASH_LISTPACK ||
                   type == RDB_TYPE_LIST_ZIPLIST || type == RDB_TYPE_ZSET_ZIPLIST ||
                   type == RDB_TYPE_HASH_ZIPLIST) {
            ObjectMap decoded;
            loadEncodedObject(reader, type, key, decoded);
            object = std::move(decoded.at(key));
        } else if (type == RDB_TYPE_LIST_QUICKLIST || type == RDB_TYPE_LIST_QUICKLIST_2) {
            object = readQuicklist(reader, type);
        } else {
            throw std::runtime_error("unsupported RDB object type " + std::to_string(type));
        }

        if (!has_pending_expire || pending_expire_at_ms > load_now_ms) {
            result.objects.emplace(key, std::move(object));
            if (has_pending_expire) {
                result.expires.emplace(key, pending_expire_at_ms);
            }
        }
        has_pending_expire = false;
        has_pending_idle = false;
        has_pending_freq = false;
    }

    if (!saw_eof || reader.position() != payload_end) {
        throw std::runtime_error("RDB EOF marker is missing or trailing data is malformed");
    }
    return result;
}

ObjectMap RdbDecoder::decodeAll() {
    return decodeAll(currentUnixMillis()).objects;
}
