#include "mini_redis/objects/HashObject.hpp"
#include "mini_redis/objects/ListObject.hpp"
#include "mini_redis/objects/SetObject.hpp"
#include "mini_redis/objects/StringObject.hpp"
#include "mini_redis/objects/ZSetObject.hpp"
#include "mini_redis/persistence/Rdb.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr uint8_t kString = 0;
constexpr uint8_t kZSet2 = 5;
constexpr uint8_t kSetIntset = 11;
constexpr uint8_t kListQuicklist2 = 18;
constexpr uint8_t kAux = 250;
constexpr uint8_t kResizeDb = 251;
constexpr uint8_t kExpireTimeMs = 252;
constexpr uint8_t kSelectDb = 254;
constexpr uint8_t kEof = 255;
constexpr uint64_t kCrc64Poly = UINT64_C(0xad93d23594c935a9);

void fail(const std::string& message) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(EXIT_FAILURE);
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

uint64_t crc64(const uint8_t* bytes, size_t length) {
    uint64_t crc = 0;
    for (size_t offset = 0; offset < length; ++offset) {
        const uint8_t byte = bytes[offset];
        for (uint8_t mask = 1; mask != 0; mask <<= 1) {
            const bool top_bit = (crc & (UINT64_C(1) << 63)) != 0;
            const bool input_bit = (byte & mask) != 0;
            crc <<= 1;
            if (top_bit != input_bit) {
                crc ^= kCrc64Poly;
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

uint64_t crc64(const std::vector<uint8_t>& bytes) {
    return crc64(bytes.data(), bytes.size());
}

void appendLe32(std::vector<uint8_t>& output, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        output.push_back(static_cast<uint8_t>(value >> (index * 8)));
    }
}

void appendLe16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8));
}

void appendLe64(std::vector<uint8_t>& output, uint64_t value) {
    for (size_t index = 0; index < 8; ++index) {
        output.push_back(static_cast<uint8_t>(value >> (index * 8)));
    }
}

void appendLen(std::vector<uint8_t>& output, uint64_t value) {
    if (value < 64) {
        output.push_back(static_cast<uint8_t>(value));
    } else if (value < 16384) {
        output.push_back(static_cast<uint8_t>(0x40 | (value >> 8)));
        output.push_back(static_cast<uint8_t>(value));
    } else if (value <= UINT32_MAX) {
        output.push_back(0x80);
        for (int shift = 24; shift >= 0; shift -= 8) {
            output.push_back(static_cast<uint8_t>(value >> shift));
        }
    } else {
        output.push_back(0x81);
        for (int shift = 56; shift >= 0; shift -= 8) {
            output.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
}

void appendString(std::vector<uint8_t>& output, const std::string& value) {
    appendLen(output, value.size());
    output.insert(output.end(), value.begin(), value.end());
}

void appendEncodedInt8String(std::vector<uint8_t>& output, int8_t value) {
    output.push_back(0xc0);
    output.push_back(static_cast<uint8_t>(value));
}

void appendLzfLiteralString(std::vector<uint8_t>& output, const std::string& value) {
    expect(!value.empty() && value.size() <= 32, "test LZF literal length is valid");
    output.push_back(0xc3);
    appendLen(output, value.size() + 1);
    appendLen(output, value.size());
    output.push_back(static_cast<uint8_t>(value.size() - 1));
    output.insert(output.end(), value.begin(), value.end());
}

std::vector<uint8_t> makeListpack(const std::vector<std::string>& values) {
    std::vector<uint8_t> output(6, 0);
    for (const auto& value : values) {
        expect(value.size() < 64, "test listpack value fits compact encoding");
        const size_t entry_length = 1 + value.size();
        output.push_back(static_cast<uint8_t>(0x80 | value.size()));
        output.insert(output.end(), value.begin(), value.end());
        output.push_back(static_cast<uint8_t>(entry_length));
    }
    output.push_back(0xff);
    const uint32_t total_bytes = static_cast<uint32_t>(output.size());
    output[0] = static_cast<uint8_t>(total_bytes);
    output[1] = static_cast<uint8_t>(total_bytes >> 8);
    output[2] = static_cast<uint8_t>(total_bytes >> 16);
    output[3] = static_cast<uint8_t>(total_bytes >> 24);
    const uint16_t count = static_cast<uint16_t>(values.size());
    output[4] = static_cast<uint8_t>(count);
    output[5] = static_cast<uint8_t>(count >> 8);
    return output;
}

void appendBinaryDouble(std::vector<uint8_t>& output, double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendLe64(output, bits);
}

std::string uniqueFilename(const std::string& suffix) {
    return "mini_redis_rdb_compat_" + std::to_string(getpid()) + suffix;
}

bool runExternalChecker(const char* executable, const std::string& filename) {
    const pid_t child = fork();
    if (child == -1) {
        return false;
    }
    if (child == 0) {
        execl(executable, executable, filename.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) == -1) {
        if (errno != EINTR) {
            return false;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void writeFile(const std::string& filename, const std::vector<uint8_t>& bytes) {
    const int fd = open(filename.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    expect(fd != -1, "open test RDB file");
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = write(fd, bytes.data() + offset, bytes.size() - offset);
        expect(written > 0, "write test RDB file");
        offset += static_cast<size_t>(written);
    }
    expect(close(fd) == 0, "close test RDB file");
}

std::vector<uint8_t> readFile(const std::string& filename) {
    const int fd = open(filename.c_str(), O_RDONLY);
    expect(fd != -1, "open generated RDB file");
    struct stat status {};
    expect(fstat(fd, &status) == 0, "stat generated RDB file");
    std::vector<uint8_t> bytes(static_cast<size_t>(status.st_size));
    size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t read_count = read(fd, bytes.data() + offset, bytes.size() - offset);
        expect(read_count > 0, "read generated RDB file");
        offset += static_cast<size_t>(read_count);
    }
    close(fd);
    return bytes;
}

void finishRdb(std::vector<uint8_t>& bytes) {
    bytes.push_back(kEof);
    appendLe64(bytes, crc64(bytes));
}

void testOfficialStyleInput() {
    std::vector<uint8_t> bytes({'R', 'E', 'D', 'I', 'S', '0', '0', '0', '9'});
    bytes.push_back(kAux);
    appendString(bytes, "redis-ver");
    appendString(bytes, "7.2.0");
    bytes.push_back(kSelectDb);
    appendLen(bytes, 0);
    bytes.push_back(kResizeDb);
    appendLen(bytes, 6);
    appendLen(bytes, 1);

    bytes.push_back(kString);
    appendString(bytes, "encoded-int");
    appendEncodedInt8String(bytes, 42);

    bytes.push_back(kString);
    appendString(bytes, "lzf");
    appendLzfLiteralString(bytes, "compressed");

    bytes.push_back(kString);
    appendString(bytes, "large");
    appendString(bytes, std::string(20000, 'x'));

    bytes.push_back(kZSet2);
    appendString(bytes, "scores");
    appendLen(bytes, 2);
    appendString(bytes, "low");
    appendBinaryDouble(bytes, 1.5);
    appendString(bytes, "high");
    appendBinaryDouble(bytes, 9.25);

    bytes.push_back(kSetIntset);
    appendString(bytes, "integers");
    std::vector<uint8_t> intset;
    appendLe32(intset, 2);
    appendLe32(intset, 3);
    appendLe16(intset, static_cast<uint16_t>(-2));
    appendLe16(intset, 0);
    appendLe16(intset, 123);
    appendString(bytes, std::string(reinterpret_cast<const char*>(intset.data()), intset.size()));

    bytes.push_back(kListQuicklist2);
    appendString(bytes, "quicklist");
    appendLen(bytes, 1);
    appendLen(bytes, 2);
    const auto listpack = makeListpack({"one", "two"});
    appendString(bytes, std::string(reinterpret_cast<const char*>(listpack.data()), listpack.size()));

    bytes.push_back(kExpireTimeMs);
    appendLe64(bytes, static_cast<uint64_t>(std::time(nullptr) - 60) * 1000);
    bytes.push_back(kString);
    appendString(bytes, "expired");
    appendString(bytes, "gone");

    finishRdb(bytes);
    const std::string filename = uniqueFilename("_official.rdb");
    writeFile(filename, bytes);
    const auto loaded = RdbEncoder::loadFromFile(filename);
    std::remove(filename.c_str());

    expect(loaded.size() == 6, "expired official key is skipped");
    const auto* integer = static_cast<const StringObject*>(loaded.at("encoded-int").get());
    const auto* lzf = static_cast<const StringObject*>(loaded.at("lzf").get());
    const auto* large = static_cast<const StringObject*>(loaded.at("large").get());
    const auto* scores = static_cast<const ZSetObject*>(loaded.at("scores").get());
    const auto* integers = static_cast<const SetObject*>(loaded.at("integers").get());
    const auto* quicklist = static_cast<const ListObject*>(loaded.at("quicklist").get());
    expect(integer->value() == "42", "official integer-encoded string loads");
    expect(lzf->value() == "compressed", "official LZF string loads");
    expect(large->value().size() == 20000, "32-bit RDB string length loads");
    expect(scores->range(0, -1, true) ==
               std::vector<std::string>({"low", "1.5", "high", "9.25"}),
           "official ZSET_2 binary scores load");
    expect(integers->contains("-2") && integers->contains("0") && integers->contains("123"),
           "official intset encoding loads");
    expect(quicklist->values() == std::vector<std::string>({"one", "two"}),
           "official QuickList2 listpack loads");
}

void testChecksumAndAtomicSave() {
    const std::string filename = uniqueFilename("_atomic.rdb");
    std::unordered_map<std::string, std::shared_ptr<RedisObject>> data;
    data.emplace("key", std::make_shared<StringObject>(std::string(20000, 'v')));
    expect(RdbEncoder::saveToFile(filename, data), "atomic save succeeds");

    const auto bytes = readFile(filename);
    expect(bytes.size() > 17, "generated RDB has header, payload and checksum");
    expect(std::string(reinterpret_cast<const char*>(bytes.data()), 9) == "REDIS0009",
           "generated RDB uses official header");
    const uint64_t stored = static_cast<uint64_t>(bytes[bytes.size() - 8]) |
        (static_cast<uint64_t>(bytes[bytes.size() - 7]) << 8) |
        (static_cast<uint64_t>(bytes[bytes.size() - 6]) << 16) |
        (static_cast<uint64_t>(bytes[bytes.size() - 5]) << 24) |
        (static_cast<uint64_t>(bytes[bytes.size() - 4]) << 32) |
        (static_cast<uint64_t>(bytes[bytes.size() - 3]) << 40) |
        (static_cast<uint64_t>(bytes[bytes.size() - 2]) << 48) |
        (static_cast<uint64_t>(bytes[bytes.size() - 1]) << 56);
    expect(stored == crc64(bytes.data(), bytes.size() - 8), "generated RDB CRC64 is valid");

    const auto loaded = RdbEncoder::loadFromFile(filename);
    expect(loaded.size() == 1, "generated RDB reloads");

    std::vector<uint8_t> corrupted = bytes;
    corrupted[12] ^= 0x01;
    const std::string corrupt_filename = uniqueFilename("_corrupt.rdb");
    writeFile(corrupt_filename, corrupted);
    const auto corrupt_loaded = RdbEncoder::loadFromFile(corrupt_filename);
    std::remove(corrupt_filename.c_str());
    expect(corrupt_loaded.empty(), "corrupt CRC RDB is rejected");
    expect(RdbEncoder::lastError().find("CRC64") != std::string::npos,
           "CRC failure exposes diagnostic message");

    const std::string directory_target = uniqueFilename("_directory");
    expect(mkdir(directory_target.c_str(), 0700) == 0, "create directory failure target");
    const auto before_failure = readFile(filename);
    expect(!RdbEncoder::saveToFile(directory_target, data), "save failure is reported");
    expect(!RdbEncoder::lastError().empty(), "save failure records diagnostic message");
    expect(readFile(filename) == before_failure, "failed save leaves existing RDB unchanged");
    expect(rmdir(directory_target.c_str()) == 0, "remove directory failure target");

    std::remove(filename.c_str());
}

} // namespace

int main() {
    testOfficialStyleInput();
    testChecksumAndAtomicSave();

    // Opt-in external validation for environments that provide the official checker.
    const char* external_checker = std::getenv("REDIS_CHECK_RDB");
    if (external_checker != nullptr && *external_checker != '\0') {
        const std::string filename = uniqueFilename("_external.rdb");
        std::unordered_map<std::string, std::shared_ptr<RedisObject>> data;
        data.emplace("string", std::make_shared<StringObject>("value"));
        data.emplace("list", std::make_shared<ListObject>());
        static_cast<ListObject*>(data.at("list").get())->push_back("one");
        data.emplace("set", std::make_shared<SetObject>());
        static_cast<SetObject*>(data.at("set").get())->add("member");
        data.emplace("zset", std::make_shared<ZSetObject>());
        static_cast<ZSetObject*>(data.at("zset").get())->add(1.5, "member");
        data.emplace("hash", std::make_shared<HashObject>());
        static_cast<HashObject*>(data.at("hash").get())->set_field("field", "value");
        expect(RdbEncoder::saveToFile(filename, data), "save RDB for external checker");
        expect(runExternalChecker(external_checker, filename),
               "official redis-check-rdb accepts generated RDB");
        std::remove(filename.c_str());
    }

    std::cout << "RDB compatibility tests passed" << std::endl;
    return EXIT_SUCCESS;
}
